# pondFilaESP

Firmware ESP32 (ESP-IDF, sem Arduino) que envia dados simulados de sensores via HTTP para o backend [pondRabbitMQServices](https://github.com/cucapcosta/pondRabbitMQServices).

## Como funciona

O ESP32 conecta no WiFi, gera leituras mockadas de temperatura e umidade, e envia via HTTP POST a cada 5 segundos para o backend. O backend enfileira no RabbitMQ e o consumer persiste no PostgreSQL.

```
ESP32                          Backend (:3420)           RabbitMQ          PostgreSQL
  |                                |                       |                  |
  |-- POST /add {temp: 27.3} ---->|                       |                  |
  |                                |-- publish "hello" -->|                  |
  |                                |                       |-- consume ----->|
  |<---- 200 OK ------------------|                       |   INSERT INTO   |
  |                                |                       |   sensor_data   |
  |-- POST /add {hum: 55.1} ----->|                       |                  |
  |        ...                     |                       |                  |
```

## Payload enviado

```json
{
  "SensorID": "ESP32-001",
  "Timestamp": "2026-01-01T00:00:00Z",
  "Type": "temperature",
  "Unit": "C",
  "IsDiscrete": false,
  "Value": 27.3
}
```

## Configuracao

Edite os `#define` no topo de `main/main.c`:

```c
#define WIFI_SSID        "MyNetwork"
#define WIFI_PASSWORD    "MyPassword"
#define SERVER_URL       "http://192.168.1.100:3420/add"
#define SENSOR_ID        "ESP32-001"
#define SEND_INTERVAL_MS 5000
```

## Build e Flash

Requer [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) instalado.

```bash
source /opt/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Estrutura

```
main/main.c  — Tudo em um arquivo: WiFi, mock de sensores, HTTP POST, loop principal
```

O codigo inclui comentarios mostrando como substituir o mock por leitura real de pino (ADC no GPIO34).

## Testar sem ESP32

Suba o backend e envie manualmente:

```bash
cd ../pondRabbitMQServices && docker-compose up -d

curl -X POST http://localhost:3420/add \
  -H "Content-Type: application/json" \
  -d '{"SensorID":"ESP32-001","Timestamp":"2026-01-01T00:00:00Z","Type":"temperature","Unit":"C","IsDiscrete":false,"Value":27.3}'
```
