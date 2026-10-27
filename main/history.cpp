#include "history.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "config.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "identity.h"

static const char *TAG = "history";

namespace {

// FR-10.6, format 2. Every slot holds 16 bytes:
//   head: bits 0..23 the sequence number of the measurements (compared
//         modulo 2^24), bits 24..31 the record type: the sensor index
//         (0..253) of a measurement, or TYPE_CONTROL. 0xFF is never used, so
//         erased flash (all bytes 0xFF) is never taken for a record.
//   body: measurement: Unix time (0 = clock not synchronized) and three
//         16-bit channels (BME280: 0.01 degC, 0.01 %RH, 0.1 hPa);
//         control record: kind and 9 bytes of data.
// Each sector starts with a header (KIND_HEADER: format version and MAC
// address), whose sequence number is that of the sector's first measurement,
// followed by one definition (KIND_SENSOR) per sensor. A sensor that changes
// gets a new definition before its next measurement. A torn write (power
// loss) fails the CRC and is skipped.
struct Record {
    uint32_t head;
    uint8_t body[10];
    uint16_t crc;  // CRC16 of the fields above
};
static_assert(sizeof(Record) == 16, "Record must stay 16 bytes");

constexpr uint32_t SEQ_MASK = 0xFFFFFF;
constexpr uint8_t TYPE_CONTROL = 0xFE;
constexpr uint8_t TYPE_ERASED = 0xFF;
constexpr uint8_t KIND_HEADER = 1;
constexpr uint8_t KIND_SENSOR = 2;
constexpr uint8_t FORMAT_VERSION = 2;
// Sensor definitions a reader keeps per sector; higher indexes read as an
// unknown sensor.
constexpr size_t MAX_SENSORS = 8;

constexpr size_t SECTOR_SIZE = 4096;  // flash erase unit
constexpr size_t RECORDS_PER_SECTOR = SECTOR_SIZE / sizeof(Record);
// Measurements in a full sector: all slots but the header and the definitions.
constexpr size_t MEASUREMENTS_PER_SECTOR = RECORDS_PER_SECTOR - 1 - identity::SENSOR_COUNT;

const esp_partition_t *s_part = nullptr;
SemaphoreHandle_t s_lock = nullptr;
size_t s_sectors = 0;
size_t s_head = 0;          // sector that receives the next record
size_t s_next_slot = 0;     // slot in s_head; RECORDS_PER_SECTOR = sector full
uint32_t s_next_seq = 1;    // sequence number of the next measurement
uint32_t s_oldest_seq = 1;  // of the oldest stored measurement
// Sensor definitions written in the head sector (FR-10.7).
identity::Sensor s_head_defs[identity::SENSOR_COUNT] = {};
bool s_head_defined[identity::SENSOR_COUNT] = {};

uint32_t seq_of(const Record &r) { return r.head & SEQ_MASK; }
uint8_t type_of(const Record &r) { return static_cast<uint8_t>(r.head >> 24); }

// a - b for sequence numbers modulo 2^24 (the stored ones span far less
// than half of that range).
int32_t seq_diff(uint32_t a, uint32_t b) { return static_cast<int32_t>((a - b) << 8) >> 8; }

uint32_t stored_count() { return (s_next_seq - s_oldest_seq) & SEQ_MASK; }

uint16_t record_crc(const Record &r)
{
    return esp_rom_crc16_le(0, reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc));
}

bool is_erased(const Record &r)
{
    const auto *bytes = reinterpret_cast<const uint8_t *>(&r);
    for (size_t i = 0; i < sizeof(Record); i++) {
        if (bytes[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

bool is_valid(const Record &r)
{
    return type_of(r) != TYPE_ERASED && r.crc == record_crc(r);
}

bool is_measurement(const Record &r) { return type_of(r) < TYPE_CONTROL && is_valid(r); }

bool is_control(const Record &r, uint8_t kind)
{
    return type_of(r) == TYPE_CONTROL && r.body[0] == kind && is_valid(r);
}

// A sector of this format starts with a header; anything else valid in the
// first slot is a record of the format before v2.0.0 (FR-10.6).
bool is_header(const Record &r) { return is_control(r, KIND_HEADER) && r.body[1] == FORMAT_VERSION; }

identity::Sensor definition_of(const Record &r)
{
    identity::Sensor s = {};
    s.index = r.body[1];
    s.type = r.body[2];
    s.address = r.body[3];
    memcpy(&s.fingerprint, &r.body[4], sizeof(s.fingerprint));
    return s;
}

bool same_sensor(const identity::Sensor &a, const identity::Sensor &b)
{
    return a.type == b.type && a.address == b.address && a.fingerprint == b.fingerprint;
}

Record make_control(uint8_t kind)
{
    Record r = {};
    r.head = (s_next_seq & SEQ_MASK) | static_cast<uint32_t>(TYPE_CONTROL) << 24;
    r.body[0] = kind;
    return r;
}

size_t sector_offset(size_t sector) { return sector * SECTOR_SIZE; }

esp_err_t read_record(size_t sector, size_t slot, Record &out)
{
    return esp_partition_read(s_part, sector_offset(sector) + slot * sizeof(Record), &out, sizeof(out));
}

// Reads a whole sector.
esp_err_t read_sector(size_t sector, Record *buf)
{
    return esp_partition_read(s_part, sector_offset(sector), buf, SECTOR_SIZE);
}

struct Lock {
    bool held;
    explicit Lock(TickType_t timeout) : held(xSemaphoreTake(s_lock, timeout) == pdTRUE) {}
    ~Lock()
    {
        if (held) {
            xSemaphoreGive(s_lock);
        }
    }
};

// Writes `r` into the next slot of the head sector. The slot is used even if
// the write failed; it is skipped on reading.
esp_err_t write_record(Record &r)
{
    r.crc = record_crc(r);
    const esp_err_t err = esp_partition_write(
        s_part, sector_offset(s_head) + s_next_slot * sizeof(Record), &r, sizeof(r));
    s_next_slot++;
    return err;
}

esp_err_t write_definition(const identity::Sensor &s)
{
    Record r = make_control(KIND_SENSOR);
    r.body[1] = s.index;
    r.body[2] = s.type;
    r.body[3] = s.address;
    memcpy(&r.body[4], &s.fingerprint, sizeof(s.fingerprint));
    const esp_err_t err = write_record(r);
    if (err == ESP_OK) {
        s_head_defs[s.index] = s;
        s_head_defined[s.index] = true;
    }
    return err;
}

// The oldest measurement is in the first sector with a header after the head
// (or in the head sector itself).
void find_oldest()
{
    s_oldest_seq = s_next_seq;
    for (size_t k = 1; k <= s_sectors; k++) {
        Record first;
        if (read_record((s_head + k) % s_sectors, 0, first) == ESP_OK && is_header(first)) {
            s_oldest_seq = seq_of(first);
            return;
        }
    }
}

// FR-7.3, FR-10.6: entering a sector; erase it, dropping its (oldest)
// records, and write the header and the sensor definitions.
esp_err_t start_sector()
{
    esp_err_t err = esp_partition_erase_range(s_part, sector_offset(s_head), SECTOR_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    for (bool &defined : s_head_defined) {
        defined = false;
    }
    Record header = make_control(KIND_HEADER);
    header.body[1] = FORMAT_VERSION;
    memcpy(&header.body[2], identity::device_mac(), 6);
    err = write_record(header);
    if (err != ESP_OK) {
        s_next_slot = RECORDS_PER_SECTOR;  // a sector without header is never read; use the next one
        return err;
    }
    for (size_t i = 0; i < identity::SENSOR_COUNT; i++) {
        identity::Sensor s;
        if (identity::sensor(i, s)) {
            write_definition(s);  // on failure, append() writes it again
        }
    }
    find_oldest();
    return ESP_OK;
}

// Locates the newest record: the sector whose header has the highest
// sequence number is the head.
esp_err_t scan()
{
    bool found = false;
    uint32_t newest_first_seq = 0;
    size_t old_sectors = 0;

    for (size_t s = 0; s < s_sectors; s++) {
        Record first;
        esp_err_t err = read_record(s, 0, first);
        if (err != ESP_OK) {
            return err;
        }
        if (!is_valid(first)) {
            continue;
        }
        if (!is_header(first)) {
            old_sectors++;
            continue;
        }
        if (!found || seq_diff(seq_of(first), newest_first_seq) > 0) {
            found = true;
            newest_first_seq = seq_of(first);
            s_head = s;
        }
    }
    if (old_sectors > 0) {
        ESP_LOGW(TAG, "%u sectors in the format before v2.0.0 are ignored and erased as the ring buffer "
                      "reaches them", static_cast<unsigned>(old_sectors));
    }

    if (!found) {
        s_head = 0;
        s_next_slot = 0;
        s_oldest_seq = s_next_seq;
        return ESP_OK;
    }

    // Inside the head sector: the next slot follows the last written one,
    // and the latest definition of each sensor is the one in force.
    std::unique_ptr<Record[]> buf(new (std::nothrow) Record[RECORDS_PER_SECTOR]);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = read_sector(s_head, buf.get());
    if (err != ESP_OK) {
        return err;
    }
    s_next_slot = 0;
    s_next_seq = newest_first_seq;
    for (size_t i = 0; i < RECORDS_PER_SECTOR; i++) {
        const Record &r = buf[i];
        if (is_erased(r)) {
            continue;
        }
        s_next_slot = i + 1;
        if (is_measurement(r) && seq_diff(seq_of(r) + 1, s_next_seq) > 0) {
            s_next_seq = (seq_of(r) + 1) & SEQ_MASK;
        } else if (is_control(r, KIND_SENSOR)) {
            const identity::Sensor def = definition_of(r);
            if (def.index < identity::SENSOR_COUNT) {
                s_head_defs[def.index] = def;
                s_head_defined[def.index] = true;
            }
        }
    }
    find_oldest();
    return ESP_OK;
}

// Channel values of a measurement; the scales are those of the BME280, the
// only sensor type so far.
history::Entry to_entry(const Record &r, const char *device_id, const char *sensor_id)
{
    uint32_t time;
    int16_t temp_centi;
    uint16_t hum_centi, press_deci;
    memcpy(&time, &r.body[0], sizeof(time));
    memcpy(&temp_centi, &r.body[4], sizeof(temp_centi));
    memcpy(&hum_centi, &r.body[6], sizeof(hum_centi));
    memcpy(&press_deci, &r.body[8], sizeof(press_deci));
    return history::Entry{seq_of(r), static_cast<time_t>(time), type_of(r), device_id, sensor_id,
                          temp_centi / 100.0f, hum_centi / 100.0f, press_deci / 10.0f};
}

void cmd_info()
{
    uint32_t count, next_seq;
    identity::Sensor defs[identity::SENSOR_COUNT];
    bool defined[identity::SENSOR_COUNT];
    {
        Lock lock(portMAX_DELAY);
        count = stored_count();
        next_seq = s_next_seq;
        memcpy(defs, s_head_defs, sizeof(defs));
        memcpy(defined, s_head_defined, sizeof(defined));
    }
    const uint32_t capacity = s_sectors * MEASUREMENTS_PER_SECTOR;
    printf("Records: %lu of %lu (%.1f%%), partition \"%s\" %lu KB\n", static_cast<unsigned long>(count),
           static_cast<unsigned long>(capacity), 100.0 * count / capacity, s_part->label,
           static_cast<unsigned long>(s_part->size / 1024));
    printf("Format %u, device %s\n", FORMAT_VERSION, identity::device_id());
    for (size_t i = 0; i < identity::SENSOR_COUNT; i++) {
        if (defined[i]) {
            char id[24];
            identity::format_sensor_id(defs[i].type, defs[i].fingerprint, id, sizeof(id));
            printf("Sensor %u: %s at i2c@0x%02x\n", static_cast<unsigned>(i), id, defs[i].address);
        }
    }
    printf("Log interval: %lu s, newest record #%lu\n",
           static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000),
           static_cast<unsigned long>((next_seq - 1) & SEQ_MASK));
}

// FR-7.4: prints the oldest-to-newest records as CSV.
void cmd_dump(uint32_t last_n)
{
    // Other log output would end up in the middle of the CSV.
    const esp_log_level_t level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);

    printf("%s", history::CSV_HEADER);
    uint32_t printed = 0;
    const esp_err_t err = history::visit(last_n, [&printed](const history::Entry &e) {
        char line[128];
        history::format_csv(e, line, sizeof(line));
        fputs(line, stdout);
        printed++;
        return true;
    });
    fflush(stdout);
    esp_log_level_set("*", level);
    if (err != ESP_OK) {
        printf("# error: %s\n", esp_err_to_name(err));
    }
    printf("# %lu records\n", static_cast<unsigned long>(printed));
}

// FR-7.5: erases every sector that holds data (also sectors of an older format).
void cmd_clear()
{
    std::unique_ptr<Record[]> buf(new (std::nothrow) Record[RECORDS_PER_SECTOR]);
    if (!buf) {
        printf("Out of memory\n");
        return;
    }
    printf("Erasing the history, this can take up to a minute...\n");
    fflush(stdout);

    Lock lock(portMAX_DELAY);
    size_t erased = 0;
    for (size_t s = 0; s < s_sectors; s++) {
        if (read_sector(s, buf.get()) != ESP_OK) {
            continue;
        }
        bool empty = true;
        for (size_t j = 0; j < RECORDS_PER_SECTOR && empty; j++) {
            empty = is_erased(buf[j]);
        }
        if (!empty && esp_partition_erase_range(s_part, sector_offset(s), SECTOR_SIZE) == ESP_OK) {
            erased++;
        }
    }
    s_head = 0;
    s_next_slot = 0;
    s_oldest_seq = s_next_seq;
    for (bool &defined : s_head_defined) {
        defined = false;
    }
    printf("History cleared (%u sectors erased)\n", static_cast<unsigned>(erased));
}

int cmd_log(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "info") == 0) {
        cmd_info();
    } else if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        cmd_dump(argc >= 3 ? strtoul(argv[2], nullptr, 10) : 0);
    } else if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        cmd_clear();
    } else {
        printf("Usage: log info | log dump [N] | log clear\n");
        return 1;
    }
    return 0;
}

}  // namespace

namespace history {

const char *const CSV_HEADER = "device_id,sensor_id,seq,time,temperature_c,humidity_rh,pressure_hpa\n";

esp_err_t init()
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      config::HISTORY_PARTITION_LABEL);
    if (s_part == nullptr) {
        ESP_LOGW(TAG, "Partition \"%s\" not found", config::HISTORY_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    s_sectors = s_part->size / SECTOR_SIZE;
    if (s_sectors < 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = scan();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%lu records stored, capacity %lu, logging every %lu s",
                 static_cast<unsigned long>(stored_count()),
                 static_cast<unsigned long>(s_sectors * MEASUREMENTS_PER_SECTOR),
                 static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    }
    return err;
}

esp_err_t append(const Bme280Data &data, time_t sample_time, uint8_t sensor_index)
{
    identity::Sensor sensor;
    if (!identity::sensor(sensor_index, sensor)) {
        return ESP_ERR_INVALID_STATE;
    }
    Record r = {};
    const uint32_t time = sample_time > 0 ? static_cast<uint32_t>(sample_time) : 0;
    const auto temp_centi = static_cast<int16_t>(lroundf(data.temperature_c * 100.0f));
    const auto hum_centi = static_cast<uint16_t>(lroundf(data.humidity_rh * 100.0f));
    const auto press_deci = static_cast<uint16_t>(lroundf(data.pressure_hpa * 10.0f));
    memcpy(&r.body[0], &time, sizeof(time));
    memcpy(&r.body[4], &temp_centi, sizeof(temp_centi));
    memcpy(&r.body[6], &hum_centi, sizeof(hum_centi));
    memcpy(&r.body[8], &press_deci, sizeof(press_deci));

    Lock lock(pdMS_TO_TICKS(config::MUTEX_TIMEOUT_MS));
    if (!lock.held) {
        return ESP_ERR_TIMEOUT;  // e.g. `log clear` in progress
    }

    // A changed sensor needs a definition before its measurement, in the
    // same sector.
    auto defined = [&]() {
        return s_head_defined[sensor_index] && same_sensor(s_head_defs[sensor_index], sensor);
    };
    const size_t needed = defined() ? 1 : 2;
    if (s_next_slot + needed > RECORDS_PER_SECTOR) {
        s_head = (s_head + 1) % s_sectors;
        s_next_slot = 0;
    }
    esp_err_t err;
    if (s_next_slot == 0 && (err = start_sector()) != ESP_OK) {
        return err;
    }
    if (!defined() && (err = write_definition(sensor)) != ESP_OK) {
        return err;
    }

    r.head = (s_next_seq & SEQ_MASK) | static_cast<uint32_t>(sensor_index) << 24;
    err = write_record(r);
    if (err == ESP_OK) {
        s_next_seq = (s_next_seq + 1) & SEQ_MASK;
    }
    return err;
}

esp_err_t visit(uint32_t last_n, const Visitor &fn)
{
    if (s_part == nullptr || s_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    std::unique_ptr<Record[]> buf(new (std::nothrow) Record[RECORDS_PER_SECTOR]);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t head;
    uint32_t newest, count;
    {
        Lock lock(portMAX_DELAY);
        head = s_head;
        newest = (s_next_seq - 1) & SEQ_MASK;
        count = stored_count();
    }
    if (last_n == 0 || last_n > count) {
        last_n = count;
    }
    if (last_n == 0) {
        return ESP_OK;
    }
    // Records outside [first_seq, newest] were written or overwritten after
    // the start of the iteration and are skipped.
    const uint32_t first_seq = (newest - last_n + 1) & SEQ_MASK;
    uint32_t last_seq = (first_seq - 1) & SEQ_MASK;

    // Records are written in order, so the window starts at most this many
    // sectors before the head: the measurements at half a sector each (room
    // for extra sensor definitions), and a margin for a partly filled head
    // sector and slots left empty by failed writes. Only these sectors are
    // read (all of them for a full dump), instead of probing every sector of
    // the partition; those that end before the window cost one record read.
    constexpr size_t MIN_PER_SECTOR = MEASUREMENTS_PER_SECTOR / 2;
    const uint64_t span = (static_cast<uint64_t>(last_n) + MIN_PER_SECTOR - 1) / MIN_PER_SECTOR + 3;
    const size_t first_i = span >= s_sectors ? 1 : s_sectors - static_cast<size_t>(span) + 1;

    char device_id[20];
    char sensor_ids[MAX_SENSORS][24];

    for (size_t i = first_i; i <= s_sectors; i++) {
        const size_t sector = (head + i) % s_sectors;  // oldest sector first
        esp_err_t err;
        {
            Lock lock(portMAX_DELAY);
            // Sectors that end before first_seq are skipped without reading them.
            Record last;
            err = read_record(sector, RECORDS_PER_SECTOR - 1, last);
            if (err == ESP_OK && is_measurement(last) && seq_diff(seq_of(last), first_seq) < 0) {
                continue;
            }
            err = read_sector(sector, buf.get());
        }
        if (err != ESP_OK || !is_header(buf[0])) {
            continue;  // unreadable, empty, or in an older format
        }
        identity::format_device_id(&buf[0].body[2], device_id, sizeof(device_id));
        for (auto &id : sensor_ids) {
            id[0] = '\0';
        }
        for (size_t j = 1; j < RECORDS_PER_SECTOR; j++) {
            const Record &r = buf[j];
            if (is_control(r, KIND_SENSOR)) {
                const identity::Sensor def = definition_of(r);
                if (def.index < MAX_SENSORS) {
                    identity::format_sensor_id(def.type, def.fingerprint, sensor_ids[def.index],
                                               sizeof(sensor_ids[def.index]));
                }
                continue;
            }
            if (!is_measurement(r) || seq_diff(seq_of(r), last_seq) <= 0 || seq_diff(seq_of(r), newest) > 0) {
                continue;
            }
            last_seq = seq_of(r);
            const uint8_t index = type_of(r);
            if (!fn(to_entry(r, device_id, index < MAX_SENSORS ? sensor_ids[index] : ""))) {
                return ESP_OK;
            }
        }
    }
    return ESP_OK;
}

int format_csv(const Entry &e, char *buf, size_t len)
{
    char stamp[24] = "";  // local time, empty if unknown
    if (e.time != 0) {
        struct tm local;
        localtime_r(&e.time, &local);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    }
    return snprintf(buf, len, "%s,%s,%lu,%s,%.2f,%.2f,%.1f\n", e.device_id, e.sensor_id,
                    static_cast<unsigned long>(e.seq), stamp, e.temperature_c, e.humidity_rh, e.pressure_hpa);
}

esp_err_t register_console_command()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "log";
    cmd.help = "Measurement history: 'info' shows usage, device and sensors, 'dump [N]' prints the last N "
               "records (all if omitted) as CSV, 'clear' erases the history";
    cmd.hint = "info | dump [N] | clear";
    cmd.func = cmd_log;
    return esp_console_cmd_register(&cmd);
}

}  // namespace history
