import streamlit as st
import pandas as pd
import plotly.express as px
from supabase import create_client

st.set_page_config(page_title="Monitoramento de Pavimento", layout="wide")

# Conexão segura usando os Secrets do Streamlit Cloud
url = st.secrets["SUPABASE_URL"]
key = st.secrets["SUPABASE_KEY"]
supabase = create_client(url, key)

st.title("🛣️ Monitoramento de Pavimento (ESP32 + IoT)")

if st.button("🔄 Atualizar Dados"):
    st.rerun()

# Busca os últimos 2000 registros ordenados do mais recente para o mais antigo
resposta = supabase.table("leituras").select("*").order("id", desc=True).limit(2000).execute()
dados = resposta.data

if not dados:
    st.warning("Nenhuma leitura encontrada no banco de dados ainda.")
    st.stop()

df = pd.DataFrame(dados)

# Filtro por dispositivo na barra lateral
dispositivos = df["device"].dropna().unique().tolist()
selecionado = st.sidebar.selectbox("Dispositivo", ["Todos"] + dispositivos)
if selecionado != "Todos":
    df = df[df["device"] == selecionado]

# Indicadores principais (KPIs)
col1, col2, col3, col4 = st.columns(4)
col1.metric("Total de Leituras", len(df))
col2.metric("RMS Médio (m/s²)", f"{df['rms'].mean():.3f}")
col3.metric("Pico Máximo RMS", f"{df['rms'].max():.3f}")
col4.metric("Velocidade Média", f"{df['speed_kmh'].mean():.1f} km/h")

# Mapa interativo colorido pela intensidade da trepidação (RMS)
st.subheader("📍 Mapa de Qualidade do Pavimento")
df_mapa = df.dropna(subset=["lat", "lon"])
df_mapa = df_mapa[(df_mapa["lat"] != 0) & (df_mapa["lon"] != 0)]

if not df_mapa.empty:
    fig_mapa = px.scatter_mapbox(
        df_mapa,
        lat="lat",
        lon="lon",
        color="rms",
        size="rms",
        color_continuous_scale=["green", "yellow", "red"],
        hover_data=["device", "speed_kmh", "rms", "ts"],
        zoom=14,
        height=500,
    )
    fig_mapa.update_layout(mapbox_style="open-street-map", margin={"r": 0, "t": 0, "l": 0, "b": 0})
    st.plotly_chart(fig_mapa, use_container_width=True)
else:
    st.info("Aguardando coordenadas GPS válidas (lat/lon diferentes de zero).")

# Gráfico de linha da vibração ao longo do tempo
st.subheader("📈 Histórico de Vibração (RMS)")
fig_linha = px.line(df.sort_values("id"), x="id", y="rms", markers=True, labels={"id": "Sequência", "rms": "RMS"})
st.plotly_chart(fig_linha, use_container_width=True)

# Tabela bruta expansível
with st.expander("Ver tabela completa de registros"):
    st.dataframe(df, use_container_width=True)
