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

static const char *TAG = "history";

namespace {

// One measurement, 16 bytes. Erased flash reads as 0xFF, so a slot with all
// bytes 0xFF is free; a torn write (power loss) fails the CRC and is skipped.
struct Record {
    uint32_t seq;         // increases by one per record, never 0xFFFFFFFF
    uint32_t time;        // Unix time, 0 = clock not synchronized
    int16_t temp_centi;   // 0.01 degC
    uint16_t hum_centi;   // 0.01 %RH
    uint16_t press_deci;  // 0.1 hPa
    uint16_t crc;         // CRC16 of the fields above
};
static_assert(sizeof(Record) == 16, "Record must stay 16 bytes");

constexpr size_t SECTOR_SIZE = 4096;  // flash erase unit
constexpr size_t RECORDS_PER_SECTOR = SECTOR_SIZE / sizeof(Record);

const esp_partition_t *s_part = nullptr;
SemaphoreHandle_t s_lock = nullptr;
size_t s_sectors = 0;
size_t s_head = 0;       // sector that receives the next record
size_t s_next_slot = 0;  // slot in s_head; RECORDS_PER_SECTOR = sector full
uint32_t s_next_seq = 1;
uint32_t s_count = 0;    // records currently stored

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
    return r.seq != 0xFFFFFFFF && r.crc == record_crc(r);
}

size_t sector_offset(size_t sector) { return sector * SECTOR_SIZE; }

esp_err_t read_record(size_t sector, size_t slot, Record &out)
{
    return esp_partition_read(s_part, sector_offset(sector) + slot * sizeof(Record), &out, sizeof(out));
}

// Reads a whole sector; used by the console commands.
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

// Locates the newest record. Records are written in order, so the first
// record of each sector tells which sector was written last.
esp_err_t scan()
{
    bool found = false;
    uint32_t newest_first_seq = 0;
    s_count = 0;

    for (size_t s = 0; s < s_sectors; s++) {
        Record first;
        esp_err_t err = read_record(s, 0, first);
        if (err != ESP_OK) {
            return err;
        }
        if (!is_valid(first)) {
            continue;
        }
        s_count += RECORDS_PER_SECTOR;  // corrected below for the head sector
        if (!found || first.seq > newest_first_seq) {
            found = true;
            newest_first_seq = first.seq;
            s_head = s;
        }
    }

    if (!found) {
        s_head = 0;
        s_next_slot = 0;
        s_next_seq = 1;
        return ESP_OK;
    }

    // Inside the head sector: the next slot follows the last written one.
    std::unique_ptr<Record[]> buf(new (std::nothrow) Record[RECORDS_PER_SECTOR]);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = read_sector(s_head, buf.get());
    if (err != ESP_OK) {
        return err;
    }
    s_next_slot = 0;
    s_next_seq = newest_first_seq + 1;
    for (size_t i = 0; i < RECORDS_PER_SECTOR; i++) {
        if (!is_erased(buf[i])) {
            s_next_slot = i + 1;
            if (is_valid(buf[i]) && buf[i].seq >= s_next_seq) {
                s_next_seq = buf[i].seq + 1;
            }
        }
    }
    s_count -= RECORDS_PER_SECTOR - s_next_slot;
    return ESP_OK;
}

void format_time(uint32_t t, char *out, size_t len)
{
    if (t == 0) {
        out[0] = '\0';
        return;
    }
    const time_t tt = t;
    struct tm local;
    localtime_r(&tt, &local);
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &local);
}

void cmd_info()
{
    uint32_t count, next_seq;
    {
        Lock lock(portMAX_DELAY);
        count = s_count;
        next_seq = s_next_seq;
    }
    const uint32_t capacity = s_sectors * RECORDS_PER_SECTOR;
    printf("Records: %lu of %lu (%.1f%%), partition \"%s\" %lu KB\n", static_cast<unsigned long>(count),
           static_cast<unsigned long>(capacity), 100.0 * count / capacity, s_part->label,
           static_cast<unsigned long>(s_part->size / 1024));
    printf("Log interval: %lu s, newest record #%lu\n",
           static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000),
           static_cast<unsigned long>(next_seq - 1));
}

// FR-7.4: prints the oldest-to-newest records as CSV. Each sector is copied
// under the lock and printed without it, so logging continues meanwhile;
// records outside the range taken at the start are skipped.
void cmd_dump(uint32_t last_n)
{
    std::unique_ptr<Record[]> buf(new (std::nothrow) Record[RECORDS_PER_SECTOR]);
    if (!buf) {
        printf("Out of memory\n");
        return;
    }

    size_t head;
    uint32_t newest, count;
    {
        Lock lock(portMAX_DELAY);
        head = s_head;
        newest = s_next_seq - 1;
        count = s_count;
    }
    if (last_n == 0 || last_n > count) {
        last_n = count;
    }
    const uint32_t first_seq = newest - last_n + 1;

    // Other log output would end up in the middle of the CSV.
    const esp_log_level_t level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);

    printf("seq,time,temperature_c,humidity_rh,pressure_hpa\n");
    uint32_t printed = 0, last_seq = first_seq - 1;
    for (size_t i = 1; i <= s_sectors && last_n > 0; i++) {
        const size_t sector = (head + i) % s_sectors;  // oldest sector first
        esp_err_t err;
        {
            Lock lock(portMAX_DELAY);
            err = read_sector(sector, buf.get());
        }
        if (err != ESP_OK) {
            continue;
        }
        for (size_t j = 0; j < RECORDS_PER_SECTOR; j++) {
            const Record &r = buf[j];
            if (!is_valid(r) || r.seq <= last_seq || r.seq > newest) {
                continue;
            }
            char stamp[24];
            format_time(r.time, stamp, sizeof(stamp));
            printf("%lu,%s,%.2f,%.2f,%.1f\n", static_cast<unsigned long>(r.seq), stamp,
                   r.temp_centi / 100.0, r.hum_centi / 100.0, r.press_deci / 10.0);
            last_seq = r.seq;
            printed++;
        }
    }
    fflush(stdout);
    esp_log_level_set("*", level);
    printf("# %lu records\n", static_cast<unsigned long>(printed));
}

// FR-7.5: erases every sector that holds data.
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
    s_count = 0;
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
                 static_cast<unsigned long>(s_count),
                 static_cast<unsigned long>(s_sectors * RECORDS_PER_SECTOR),
                 static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    }
    return err;
}

esp_err_t append(const Bme280Data &data, time_t sample_time)
{
    Record r = {};
    r.time = sample_time > 0 ? static_cast<uint32_t>(sample_time) : 0;
    r.temp_centi = static_cast<int16_t>(lroundf(data.temperature_c * 100.0f));
    r.hum_centi = static_cast<uint16_t>(lroundf(data.humidity_rh * 100.0f));
    r.press_deci = static_cast<uint16_t>(lroundf(data.pressure_hpa * 10.0f));

    Lock lock(pdMS_TO_TICKS(config::MUTEX_TIMEOUT_MS));
    if (!lock.held) {
        return ESP_ERR_TIMEOUT;  // e.g. `log clear` in progress
    }

    if (s_next_slot == RECORDS_PER_SECTOR) {
        s_head = (s_head + 1) % s_sectors;
        s_next_slot = 0;
    }
    if (s_next_slot == 0) {
        // FR-7.3: entering a sector; erase it, dropping its (oldest) records.
        Record first;
        if (read_record(s_head, 0, first) == ESP_OK && is_valid(first)) {
            s_count -= RECORDS_PER_SECTOR;
        }
        esp_err_t err = esp_partition_erase_range(s_part, sector_offset(s_head), SECTOR_SIZE);
        if (err != ESP_OK) {
            return err;
        }
    }

    r.seq = s_next_seq;
    r.crc = record_crc(r);
    const esp_err_t err = esp_partition_write(
        s_part, sector_offset(s_head) + s_next_slot * sizeof(Record), &r, sizeof(r));
    // The slot is used even if the write failed; it is skipped on reading.
    s_next_slot++;
    if (err == ESP_OK) {
        s_next_seq++;
        s_count++;
    }
    return err;
}

esp_err_t register_console_command()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "log";
    cmd.help = "Measurement history: 'info' shows usage, 'dump [N]' prints the last N "
               "records (all if omitted) as CSV, 'clear' erases the history";
    cmd.hint = "info | dump [N] | clear";
    cmd.func = cmd_log;
    return esp_console_cmd_register(&cmd);
}

}  // namespace history
