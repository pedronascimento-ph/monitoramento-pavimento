import streamlit as st
import pandas as pd
import plotly.express as px
from supabase import create_client

st.set_page_config(page_title="Monitoramento de Pavimento", layout="wide")

url = st.secrets["SUPABASE_URL"]
key = st.secrets["SUPABASE_KEY"]
supabase = create_client(url, key)

st.title("🛣️ Monitoramento de Pavimento (ESP32 + IoT)")

if st.button("🔄 Atualizar Dados"):
    st.rerun()

resposta = supabase.table("leituras").select("*").order("id", desc=True).limit(2000).execute()
dados = resposta.data

if not dados:
    st.warning("Nenhuma leitura encontrada no banco de dados ainda.")
    st.stop()

df = pd.DataFrame(dados)

dispositivos = df["device"].dropna().unique().tolist()
selecionado = st.sidebar.selectbox("Dispositivo", ["Todos"] + dispositivos)
if selecionado != "Todos":
    df = df[df["device"] == selecionado]

df = df[df["speed_kmh"] >= 5.0].copy()

def classificar_pavimento(rms):
    if rms < 1.30:
        return "Bom"
    elif rms <= 1.80:
        return "Regular"
    else:
        return "Ruim"

df["qualidade"] = df["rms"].apply(classificar_pavimento)

col1, col2, col3, col4 = st.columns(4)
col1.metric("Total de Leituras", len(df))
col2.metric("RMS Médio (m/s²)", f"{df['rms'].mean():.3f}")
col3.metric("Pico Máximo RMS", f"{df['rms'].max():.3f}")
col4.metric("Velocidade Média", f"{df['speed_kmh'].mean():.1f} km/h")

st.subheader("📍 Mapa de Qualidade do Pavimento")
df_mapa = df.dropna(subset=["lat", "lon"])
df_mapa = df_mapa[(df_mapa["lat"] != 0) & (df_mapa["lon"] != 0)]

# Coordenadas iniciais fixas (Campo Grande -MS) e nível de zoom
LAT_INICIAL = df_mapa["lat"].mean() if not df_mapa.empty else -20.4331
LON_INICIAL = df_mapa["lon"].mean() if not df_mapa.empty else -54.5839
ZOOM_INICIAL = 17

if not df_mapa.empty:
    fig_mapa = px.scatter_map(
        df_mapa,
        lat="lat",
        lon="lon",
        color="qualidade",
        size="rms",
        color_discrete_map={"Bom": "green", "Regular": "yellow", "Ruim": "red"},
        category_orders={"qualidade": ["Bom", "Regular", "Ruim"]},
        hover_data=["device", "speed_kmh", "rms", "ts"],
        center={"lat": LAT_INICIAL, "lon": LON_INICIAL},
        zoom=ZOOM_INICIAL,
        height=500,
    )
    fig_mapa.update_layout(map_style="open-street-map", margin={"r": 0, "t": 0, "l": 0, "b": 0})
    st.plotly_chart(fig_mapa, use_container_width=True)
else:
    st.info("Aguardando coordenadas GPS válidas (lat/lon diferentes de zero).")

st.subheader("📈 Histórico de Vibração (RMS)")
fig_linha = px.line(df.sort_values("id"), x="id", y="rms", markers=True, labels={"id": "Sequência", "rms": "RMS"})
st.plotly_chart(fig_linha, use_container_width=True)

with st.expander("Ver tabela completa de registros"):
    st.dataframe(df, use_container_width=True)
