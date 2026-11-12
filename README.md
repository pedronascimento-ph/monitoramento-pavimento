# 🛣️ Sistema IoT de Monitoramento de Pavimento

Protótipo de telemetria veicular em tempo real para avaliação da qualidade e irregularidade de vias urbanas e rodovias utilizando **ESP32**, acelerômetro, **GPS** e nuvem **IoT**.

---

## 📌 Visão Geral do Projeto

O sistema coleta dados de vibração vertical e geolocalização a **100 Hz**, calcula o **RMS (Root Mean Square)** da aceleração em janelas de 1 segundo e transmite os pacotes de forma assíncrona via **MQTT (TLS)** para armazenamento em banco de dados na nuvem e visualização interativa em mapa.

### ✨ Principais Funcionalidades
* **Processamento Dual-Core no ESP32:** O Núcleo 1 realiza a leitura contínua dos sensores sem interrupções, enquanto o Núcleo 0 gerencia a conexão Wi-Fi e o envio MQTT.
* **Buffer Offline Inteligente:** Fila interna em memória capaz de armazenar até ~500 registros (~8 minutos de coleta) caso a conexão com a internet oscile, retransmitindo automaticamente quando o sinal retorna.
* **Comunicação Segura:** Transmissão criptografada via **MQTT sobre TLS/SSL (porta 8883)**.
* **Dashboard Interativo Online:** Mapa de calor/dispersão com classificação visual da trepidação e histórico temporal.

---

## 🏗️ Arquitetura do Sistema

```text
[ESP32 + MPU6050 + GPS] 
        │  (MQTT over TLS - Porta 8883)
        ▼
[EMQX Cloud Serverless] 
        │  (Data Integration / Webhook POST)
        ▼
[Supabase (PostgreSQL)] 
        │  (REST API / Python Client)
        ▼
[Streamlit Community Cloud (Dashboard)]
```

---

## 🔌 Hardware e Ligações (ESP32)

| Componente | Pino no Componente | Pino no ESP32 | Observação |
| :--- | :--- | :--- | :--- |
| **Acelerômetro (MPU6050/6500)** | `SDA` | `GPIO 21` | Barramento I2C |
| **Acelerômetro (MPU6050/6500)** | `SCL` | `GPIO 22` | Barramento I2C |
| **Módulo GPS** | `TX` | `GPIO 16` | UART2 RX (9600 bps) |
| **LED de Status** | Ânodo (+) | `GPIO 18` | Usar resistor em série para GND |
| **Alimentação Geral** | `VCC` / `GND` | `3V3` / `GND` | Operação em 3,3 V |

### 💡 Código de Sinalização do LED
* **Aceso fixo:** Aguardando sinal válido de satélites (GPS Fix).
* **1 piscada/segundo:** Coletando dados e enviando para a nuvem normalmente.
* **2 piscadas/segundo:** Coletando dados sem internet (salvando na fila local).
* **Piscando rápido:** Falha de leitura no sensor MPU (tentando reconexão I2C).

---

## 🗂️ Estrutura do Repositório

* `app.py`: Aplicação web desenvolvida em Python com **Streamlit** e **Plotly** para visualização dos indicadores, gráficos e mapa interativo.
* `requirements.txt`: Lista de dependências Python utilizadas para o deploy no Streamlit Cloud.
* `README.md`: Documentação técnica do projeto.

---

## 🚀 Como Executar o Dashboard

1. **Banco de Dados (Supabase):** Crie a tabela `leituras` com as colunas `device`, `ts`, `ms`, `lat`, `lon`, `speed_kmh`, `sats` e `rms`, habilitando as políticas de `INSERT` e `SELECT` via RLS.
2. **Broker MQTT (EMQX Cloud):** Configure uma regra SQL encaminhando os tópicos `pavimento/+/leitura` via Webhook para a API REST do Supabase.
3. **Deploy no Streamlit Cloud:** Conecte este repositório em [share.streamlit.io](https://streamlit.io) e adicione suas credenciais em **Settings > Secrets**:
   ```toml
   SUPABASE_URL = "https://supabase.co"
   SUPABASE_KEY = "sua_publishable_key"
   ```
