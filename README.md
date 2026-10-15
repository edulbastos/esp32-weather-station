# ESP32 Weather Station

Estação meteorológica com ESP32 que lê **temperatura**, **umidade** e **pressão atmosférica** de um sensor BME280 e mostra os valores em tempo real no display OLED SSD1306 de 0,96" integrado à placa AE-BTG-ESP32.

Escrito em C++ com **ESP-IDF v6.1** e **FreeRTOS**, sem bibliotecas externas: os drivers do BME280 e do SSD1306 foram implementados diretamente sobre o driver I2C do ESP-IDF.

```
┌────────────────────────────┐
│      WEATHER STATION       │  ← faixa amarela do display
│ TEMP          23.4 °C      │
│ HUM           56.1 %RH     │
│ PRES        1012.8 hPa     │
└────────────────────────────┘
```

## Hardware

| Componente | Descrição |
|---|---|
| Placa | AE-BTG-ESP32 com OLED SSD1306 0,96" (128x64, I2C) integrado |
| Sensor | BME280 (temperatura, umidade, pressão), I2C |
| Alimentação | 3,3 V |

### Ligações

O BME280 compartilha o barramento I2C com o display da placa:

| BME280 | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

Endereços I2C padrão: BME280 em `0x76` (SDO no GND; use `0x77` se SDO estiver no VCC) e SSD1306 em `0x3C`. O pinout completo da placa está em [pinout.png](pinout.png).

O LED azul da placa (GPIO 2) pisca quando o sistema entra em modo de erro.

## Como compilar e gravar

Pré-requisito: [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/) instalado.

```bash
# 1. Ativar o ambiente do ESP-IDF
source ~/.espressif/tools/activate_idf_v6.1.sh

# 2. Definir o alvo (só na primeira vez)
idf.py set-target esp32

# 3. Compilar
idf.py build

# 4. Gravar e abrir o monitor serial (ajuste a porta)
idf.py -p /dev/ttyUSB0 flash monitor
```

O monitor serial roda a 115200 baud e mostra as leituras, por exemplo:

```
I (1234) weather: T=23.41 C  H=56.12 %RH  P=1012.84 hPa  Alt~3 m
```

## Configuração

Todos os parâmetros ajustáveis ficam em [main/config.h](main/config.h):

- pinos, frequência e porta do I2C;
- endereços do BME280 e do SSD1306;
- tamanho (128x64 ou 128x32), contraste e faixa amarela do display;
- pressão ao nível do mar usada na estimativa de altitude;
- períodos, pilhas e prioridades das tarefas.

## Arquitetura

O firmware roda duas tarefas FreeRTOS:

- **`taskMeasure`**: lê o BME280 a cada 2 s.
- **`taskDisplay`**: atualiza o display a cada 1 s.

O mutex `i2cMutex` controla o acesso ao barramento I2C, e o `dataMutex` protege os dados compartilhados entre as tarefas. Depois de 3 falhas seguidas, o dispositivo é reinicializado automaticamente. Se o display ou o sensor não iniciarem no boot, o sistema para e sinaliza o erro pelo LED e pelo console serial.

```
main/
├── main.cpp      # app_main, inicialização e tarefas FreeRTOS
├── config.h      # configuração central
├── bme280.*      # driver do BME280 (fórmulas de compensação da Bosch)
├── ssd1306.*     # driver do SSD1306 com frame buffer local
└── font5x7.h     # fonte 5x7 (ASCII + símbolo de grau)
```

A especificação funcional completa está em [esp32-weather-station.md](esp32-weather-station.md).

## Próximos passos

- Envio dos dados por Wi-Fi para um servidor ou serviço na nuvem
- Registro histórico em cartão SD ou na flash interna
- Servidor web local para consultar as leituras
- Calibração do sensor
- Relógio de tempo real (RTC) para registrar data e hora
