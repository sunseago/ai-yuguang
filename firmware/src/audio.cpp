#include "audio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s.h>

#include <math.h>
#include <string.h>

#include "board_config.h"

#include "audio/es8311.h"

namespace {

// ES8311 要求 MCLK = 采样率 × 256，16kHz 下就是 4.096MHz
constexpr int kSampleRate = 16000;
constexpr int kMclk = kSampleRate * 256;
constexpr int kBlockFrames = 128;  // 一次喂给 DMA 的帧数（立体声 → 512 字节）
constexpr float kPi = 3.14159265f;

es8311_handle_t s_codec = nullptr;
bool s_ok = false;
uint8_t s_mode = DP_SND_BOTH;
QueueHandle_t s_queue = nullptr;
int16_t s_block[kBlockFrames * 2];  // 左右声道交错

// 两种音效的时长（秒）
constexpr float kDropDur = 0.16f;
constexpr float kWaveDur = 1.70f;

// ---------------- 波形合成 ----------------

// 水滴：音高快速上滑（像气泡破裂），包络掉得很快
float dropSample(float t, float& phase) {
  const float f = 700.0f + 900.0f * (1.0f - expf(-t / 0.018f));
  phase += 2.0f * kPi * f / kSampleRate;
  const float attack = 1.0f - expf(-t / 0.0015f);
  const float env = attack * expf(-t / 0.035f);
  return sinf(phase) * env * 0.55f;
}

// 海浪：白噪声过一阶低通（约 400Hz），慢起慢落，再叠一点 0.8Hz 的起伏
static uint32_t s_rng = 0x2545F491u;
float whiteNoise() {
  s_rng = s_rng * 1664525u + 1013904223u;
  return (float)(s_rng >> 8) * (1.0f / 8388608.0f) - 1.0f;  // -1..1
}
float waveSample(float t, float& lp) {
  const float n = whiteNoise();
  lp += 0.16f * (n - lp);
  float env = (t < 0.55f) ? (t / 0.55f) : expf(-(t - 0.55f) / 0.45f);
  env *= 1.0f + 0.22f * sinf(2.0f * kPi * 0.8f * t);
  return lp * env * 1.7f;
}

// 一帧：算一个单声道样本
struct Gen {
  uint8_t kind;
  float t = 0;
  float phase = 0;
  float lp = 0;
  float dur;

  void init(uint8_t k) {
    kind = k;
    t = 0;
    phase = 0;
    lp = 0;
    dur = (k == DP_SND_WAVE) ? kWaveDur : kDropDur;
  }
  bool done() const { return t >= dur; }
  float next() {
    const float v = (kind == DP_SND_WAVE) ? waveSample(t, lp) : dropSample(t, phase);
    t += 1.0f / kSampleRate;
    return v;
  }
};

void playBlocking(uint8_t kind) {
  Gen g;
  g.init(kind);
  int idx = 0;
  while (!g.done()) {
    const float v = g.next();
    s_block[idx++] = (int16_t)(v * 0.55f * 32767.0f);
    s_block[idx++] = s_block[idx - 1];
    if (idx >= kBlockFrames * 2) {
      size_t written = 0;
      i2s_write(I2S_NUM_0, s_block, sizeof(s_block), &written, portMAX_DELAY);
      idx = 0;
    }
  }
  if (idx > 0) {
    size_t written = 0;
    i2s_write(I2S_NUM_0, s_block, idx * sizeof(int16_t), &written, portMAX_DELAY);
  }
  // 补一小段静音，避免下一次发音带上尾部噪声
  memset(s_block, 0, sizeof(s_block));
  size_t written = 0;
  i2s_write(I2S_NUM_0, s_block, sizeof(s_block), &written, 100 / portTICK_PERIOD_MS);
}

void audioTask(void*) {
  for (;;) {
    uint8_t kind = 0;
    if (xQueueReceive(s_queue, &kind, portMAX_DELAY) != pdTRUE) continue;
    if (!s_ok || s_mode == DP_SND_OFF || kind == DP_SND_OFF) continue;
    playBlocking(kind);
  }
}

bool i2sUp() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB);
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = kBlockFrames;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = AUDIO_MCLK;
  pins.bck_io_num = AUDIO_BCLK;
  pins.ws_io_num = AUDIO_LRCK;
  pins.data_out_num = AUDIO_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

bool codecUp() {
  // 复用 Wire 已经打开的 I2C0（ES8311 与触摸/IMU/PMU 同一条总线，地址 0x18）
  s_codec = es8311_create(0, ES8311_ADDRRES_0);
  if (!s_codec) return false;

  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false;
  clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = kMclk;
  clk.sample_frequency = kSampleRate;
  if (es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_sample_frequency_config(s_codec, kMclk, kSampleRate) != ESP_OK) return false;
  es8311_voice_volume_set(s_codec, 62, nullptr);
  es8311_voice_mute(s_codec, false);
  return true;
}

uint8_t loadMode() {
  Preferences p;
  p.begin("deskpulse", true);
  const uint8_t v = (uint8_t)p.getUChar("snd", DP_SND_BOTH);
  p.end();
  return v > DP_SND_BOTH ? DP_SND_BOTH : v;
}

}  // namespace

bool audioInit() {
  s_mode = loadMode();

  // 功放使能先拉高：NS4150B 的 CTRL 脚，拉高才出声
  pinMode(AUDIO_PA, OUTPUT);
  digitalWrite(AUDIO_PA, HIGH);

  if (!i2sUp()) {
    Serial.println("[dp] audio=FAILED (i2s install)");
    return false;
  }
  if (!codecUp()) {
    Serial.println("[dp] audio=FAILED (es8311 init)");
    return false;
  }

  s_queue = xQueueCreate(4, sizeof(uint8_t));
  if (!s_queue || xTaskCreate(audioTask, "dp_audio", 3072, nullptr, 3, nullptr) != pdPASS) {
    Serial.println("[dp] audio=FAILED (task)");
    return false;
  }
  s_ok = true;
  Serial.printf("[dp] audio=ES8311 ok, mode=%s\n", audioModeLabel(s_mode));
  return true;
}

uint8_t audioMode() { return s_mode; }

void audioSetMode(uint8_t mode) {
  s_mode = mode > DP_SND_BOTH ? DP_SND_BOTH : mode;
  Preferences p;
  p.begin("deskpulse", false);
  p.putUChar("snd", s_mode);
  p.end();
}

uint8_t audioCycleMode() {
  audioSetMode((uint8_t)((s_mode + 1) % 4));
  return s_mode;
}

void audioPlay(uint8_t kind) {
  if (!s_ok || !s_queue) return;
  xQueueSend(s_queue, &kind, 0);  // 队列满了就丢，提示音不值得阻塞主循环
}

void audioPlayEvent(bool complete) {
  switch (s_mode) {
    case DP_SND_DROP:
      audioPlay(DP_SND_DROP);
      break;
    case DP_SND_WAVE:
      audioPlay(DP_SND_WAVE);
      break;
    case DP_SND_BOTH:
      // 弹窗用水滴（短促、催你）、完成用海浪（舒展、收尾）
      audioPlay(complete ? DP_SND_WAVE : DP_SND_DROP);
      break;
    default:
      break;  // 关
  }
}

const char* audioModeLabel(uint8_t mode) {
  switch (mode) {
    case DP_SND_DROP: return "水滴";
    case DP_SND_WAVE: return "海浪";
    case DP_SND_BOTH: return "水滴+海浪";
    default: return "关";
  }
}
