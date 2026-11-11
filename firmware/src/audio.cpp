#include "audio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s.h>

#include <math.h>
#include <string.h>

#include "board_config.h"

#include "audio/es8311.h"
#include "soundnames.h"

namespace {

// ES8311 要求 MCLK = 采样率 × 256，16kHz 下就是 4.096MHz
constexpr int kSampleRate = 16000;
constexpr int kMclk = kSampleRate * 256;
constexpr int kBlockFrames = 128;  // 一次喂给 DMA 的帧数（立体声 → 512 字节）
constexpr float kPi = 3.14159265f;

es8311_handle_t s_codec = nullptr;
bool s_ok = false;
uint8_t s_alert = DP_SND_DROP;
uint8_t s_done = DP_SND_WAVE;
uint8_t s_vol = 65;
QueueHandle_t s_queue = nullptr;
int16_t s_block[kBlockFrames * 2];  // 左右声道交错

// ---------------- 乐器音色：基频 + 分音表 ----------------
//
// 每个分音：相对基频的比例 ratio、幅度 amp、衰减时间常数 tau（秒）。
// 音色差别基本都在这张表里：比例决定音高关系（整数=谐波，非整数=金属感），
// tau 决定余音（越短越像敲木头，越长越像钟）。
struct Partial {
  float ratio;
  float amp;
  float tau;
};

// 木琴：高次分音衰减极快，敲击感强
const Partial kXylo[] = {{1.00f, 1.00f, 0.16f}, {3.00f, 0.38f, 0.07f}, {3.93f, 0.24f, 0.05f}};
// 马林巴：比木琴柔和，保留 3.93 倍的非谐分音（木条特征）
const Partial kMarimba[] = {{1.00f, 1.00f, 0.40f}, {3.93f, 0.30f, 0.16f}, {9.20f, 0.10f, 0.06f}};
// 软铃：非整数分音 + 慢衰减，柔和但有钟的余韵
const Partial kSoftBell[] = {
    {1.00f, 1.00f, 0.75f}, {2.00f, 0.30f, 0.50f}, {2.76f, 0.14f, 0.32f}, {5.40f, 0.05f, 0.18f}};
// 钢琴：整数谐波逐个衰减，二三次最明显
const Partial kPiano[] = {{1.00f, 1.00f, 0.70f},
                          {2.00f, 0.32f, 0.48f},
                          {3.00f, 0.14f, 0.30f},
                          {4.00f, 0.06f, 0.20f},
                          {5.00f, 0.03f, 0.14f}};
// 竖琴：像钢琴但更"空"，高次谐波收得更快
const Partial kHarp[] = {{1.00f, 1.00f, 0.75f},
                         {2.00f, 0.50f, 0.50f},
                         {3.00f, 0.28f, 0.34f},
                         {4.00f, 0.16f, 0.24f},
                         {5.00f, 0.09f, 0.16f}};
// 八音盒：基频很高 + 5.4 倍金属分音，衰减极快 = 脆
const Partial kMusicBox[] = {{1.00f, 1.00f, 0.30f}, {5.40f, 0.32f, 0.11f}, {8.90f, 0.12f, 0.06f}};
// 风铃单片：钟类分音，余音长
const Partial kChime[] = {{1.00f, 1.00f, 0.80f}, {2.76f, 0.20f, 0.40f}, {5.40f, 0.05f, 0.18f}};

struct Voice {
  float f0;
  float dur;
  float attack;  // 起振时间常数，越小越硬
  const Partial* parts;
  uint8_t n;
  float gain;
};

const Voice kVoiceXylo = {1046.0f, 0.45f, 0.0008f, kXylo, 3, 0.34f};
const Voice kVoiceMarimba = {523.0f, 0.75f, 0.0015f, kMarimba, 3, 0.34f};
const Voice kVoiceSoftBell = {587.0f, 1.00f, 0.0070f, kSoftBell, 4, 0.30f};
const Voice kVoicePiano = {392.0f, 1.00f, 0.0016f, kPiano, 5, 0.28f};
const Voice kVoiceHarp = {440.0f, 1.00f, 0.0020f, kHarp, 5, 0.26f};
const Voice kVoiceMusicBox = {1760.0f, 0.60f, 0.0006f, kMusicBox, 3, 0.30f};

// 叠加式合成：每个分音独立衰减；相位用绝对时间算，不需要逐分音维护状态
float additive(float t, const Voice& v) {
  const float att = 1.0f - expf(-t / v.attack);
  float sum = 0;
  for (uint8_t i = 0; i < v.n; i++) {
    const Partial& p = v.parts[i];
    const float env = expf(-t / p.tau);
    sum += sinf(2.0f * kPi * p.ratio * v.f0 * t + (float)i * 0.7f) * p.amp * env;
  }
  return sum * att * v.gain;
}

// 水滴：音高快速上滑（像气泡破裂），包络掉得很快
float dropSample(float t, float& phase) {
  const float f = 700.0f + 900.0f * (1.0f - expf(-t / 0.018f));
  phase += 2.0f * kPi * f / kSampleRate;
  const float env = (1.0f - expf(-t / 0.0015f)) * expf(-t / 0.035f);
  return sinf(phase) * env * 0.55f;
}

// 气泡：比水滴低、带 26Hz 的"咕嘟"颤音，第二声 110ms 后接上
float bubbleSample(float t, float& phase) {
  const float f = 300.0f + 620.0f * (1.0f - expf(-t / 0.022f));
  phase += 2.0f * kPi * f / kSampleRate;
  const float trem = 0.72f + 0.28f * sinf(2.0f * kPi * 26.0f * t);
  float env = (1.0f - expf(-t / 0.002f)) * expf(-t / 0.055f);
  if (t > 0.11f) env = fmaxf(env, expf(-(t - 0.11f) / 0.045f) * 0.6f);
  return sinf(phase) * env * trem * 0.6f;
}

// 海浪：白噪声过一阶低通，慢起慢落，叠一点 0.8Hz 起伏
uint32_t s_rng = 0x2545F491u;
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

// 轻风铃：5 片错开敲击，音高不同，像一阵风拂过
constexpr int kChimeN = 5;
const float kChimeF[kChimeN] = {1175.0f, 1319.0f, 1568.0f, 1760.0f, 2093.0f};
const float kChimeAt[kChimeN] = {0.00f, 0.16f, 0.34f, 0.52f, 0.72f};
float chimeSample(float t) {
  float sum = 0;
  for (int i = 0; i < kChimeN; i++) {
    const float dt = t - kChimeAt[i];
    if (dt <= 0) continue;
    const Voice v = {kChimeF[i], 2.0f, 0.0025f, kChime, 3, 0.15f};
    sum += additive(dt, v);
  }
  return sum;
}

// 雨声：白噪声拆成高低两段做带通（约 500Hz~3kHz 的沙沙声），
// 再叠上按随机间隔触发的"水珠"短促衰减 —— 只有噪声会像电视雪花，没有雨感。
float rainSample(float t, float& lpFast, float& lpSlow, float& tick, float& nextTick) {
  const float n = whiteNoise();
  lpFast += 0.45f * (n - lpFast);
  lpSlow += 0.05f * (n - lpSlow);
  const float hiss = (lpFast - lpSlow) * 1.1f;

  tick *= 0.986f;  // 约 4.5ms 衰减
  if (t >= nextTick) {
    tick = 1.0f;
    s_rng = s_rng * 1664525u + 1013904223u;
    nextTick = t + 0.012f + (float)(s_rng >> 16) / 65536.0f * 0.05f;
  }
  // 总体包络：0.15s 淡入，末尾 0.5s 淡出（时长写死，避免依赖后面才定义的 durOf）
  const float kRainDur = 2.40f;
  float env = (t < 0.15f) ? t / 0.15f : 1.0f;
  if (t > kRainDur - 0.5f) env *= (kRainDur - t) / 0.5f;
  return (hiss * 0.55f + hiss * tick * 1.3f) * env;
}

// 远处鸟鸣：三声短促啁啾，音高上下扫 + 38Hz 颤音；整体音量压低表示"远"
float birdSample(float t, float& phase) {
  static const float at[3] = {0.02f, 0.20f, 0.56f};
  static const float f0[3] = {2900.0f, 3400.0f, 2700.0f};
  static const float f1[3] = {3700.0f, 2600.0f, 3500.0f};
  static const float du[3] = {0.085f, 0.100f, 0.090f};
  static const float am[3] = {0.50f, 0.42f, 0.34f};
  float sum = 0;
  for (int i = 0; i < 3; i++) {
    const float u = (t - at[i]) / du[i];
    if (u <= 0.0f || u >= 1.0f) continue;
    const float f = f0[i] + (f1[i] - f0[i]) * u + 200.0f * sinf(2.0f * kPi * 38.0f * u);
    phase += 2.0f * kPi * f / kSampleRate;
    const float env = powf(sinf(kPi * u), 0.7f);
    sum += sinf(phase) * env * am[i];
  }
  return sum * 0.5f;
}

float durOf(uint8_t kind) {
  switch (kind) {
    case DP_SND_WAVE: return 1.70f;
    case DP_SND_CHIME: return 1.60f;
    case DP_SND_RAIN: return 2.40f;
    case DP_SND_BIRD: return 1.00f;
    case DP_SND_SOFTBELL: return kVoiceSoftBell.dur;
    case DP_SND_PIANO: return kVoicePiano.dur;
    case DP_SND_HARP: return kVoiceHarp.dur;
    case DP_SND_MARIMBA: return kVoiceMarimba.dur;
    case DP_SND_MUSICBOX: return kVoiceMusicBox.dur;
    case DP_SND_XYLO: return kVoiceXylo.dur;
    case DP_SND_BUBBLE: return 0.22f;
    default: return 0.16f;  // 水滴
  }
}

struct Gen {
  uint8_t kind;
  float t = 0;
  float phase = 0;
  float lp = 0;
  float lp2 = 0;
  float tick = 0;
  float nextTick = 0.05f;
  float dur;

  void init(uint8_t k) {
    kind = k;
    t = 0;
    phase = 0;
    lp = 0;
    lp2 = 0;
    tick = 0;
    nextTick = 0.05f;
    dur = durOf(k);
  }
  bool done() const { return t >= dur; }
  float next() {
    float v;
    switch (kind) {
      case DP_SND_WAVE: v = waveSample(t, lp); break;
      case DP_SND_CHIME: v = chimeSample(t); break;
      case DP_SND_RAIN: v = rainSample(t, lp, lp2, tick, nextTick); break;
      case DP_SND_BIRD: v = birdSample(t, phase); break;
      case DP_SND_BUBBLE: v = bubbleSample(t, phase); break;
      case DP_SND_XYLO: v = additive(t, kVoiceXylo); break;
      case DP_SND_MARIMBA: v = additive(t, kVoiceMarimba); break;
      case DP_SND_SOFTBELL: v = additive(t, kVoiceSoftBell); break;
      case DP_SND_PIANO: v = additive(t, kVoicePiano); break;
      case DP_SND_HARP: v = additive(t, kVoiceHarp); break;
      case DP_SND_MUSICBOX: v = additive(t, kVoiceMusicBox); break;
      default: v = dropSample(t, phase); break;
    }
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
    // 主音量由 audioVolume() 控制，上限压到 0.8 留点余量避免削顶
    const float gain = (float)s_vol / 100.0f * 0.8f;
    int32_t s = (int32_t)(v * gain * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    s_block[idx++] = (int16_t)s;
    s_block[idx++] = (int16_t)s;
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
    if (!s_ok || kind == DP_SND_OFF) continue;
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

void saveSlot(const char* key, uint8_t v) {
  Preferences p;
  p.begin("deskpulse", false);
  p.putUChar(key, v);
  p.end();
}

// 读两个槽。旧版本只有一个 "snd"（其中 3 表示"水滴+海浪"），这里做一次迁移。
void loadSlots() {
  Preferences p;
  p.begin("deskpulse", true);
  if (p.isKey("sndA")) {
    s_alert = p.getUChar("sndA", DP_SND_DROP);
    s_done = p.getUChar("sndD", DP_SND_WAVE);
  } else {
    const uint8_t legacy = p.getUChar("snd", 3);  // 旧默认 = 水滴+海浪
    if (legacy == 3) {
      s_alert = DP_SND_DROP;
      s_done = DP_SND_WAVE;
    } else {
      s_alert = legacy;
      s_done = legacy;
    }
  }
  s_vol = p.getUChar("vol", 65);
  if (s_vol > 100) s_vol = 100;
  p.end();
  if (s_alert >= DP_SND_COUNT) s_alert = DP_SND_DROP;
  if (s_done >= DP_SND_COUNT) s_done = DP_SND_WAVE;
  s_vol = p.getUChar("vol", 65);
  if (s_vol > 100) s_vol = 100;

  // 写回新键，下次就不用再迁移
  saveSlot("sndA", s_alert);
  saveSlot("sndD", s_done);
}

}  // namespace

bool audioInit() {
  loadSlots();

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
  Serial.printf("[dp] audio=ES8311 ok, alert=%s done=%s\n", audioModeLabel(s_alert),
                audioModeLabel(s_done));
  return true;
}

uint8_t audioAlertMode() { return s_alert; }
uint8_t audioDoneMode() { return s_done; }

void audioSetAlertMode(uint8_t mode) {
  s_alert = mode >= DP_SND_COUNT ? DP_SND_OFF : mode;
  saveSlot("sndA", s_alert);
}

void audioSetDoneMode(uint8_t mode) {
  s_done = mode >= DP_SND_COUNT ? DP_SND_OFF : mode;
  saveSlot("sndD", s_done);
}

uint8_t audioVolume() { return s_vol; }

void audioSetVolume(uint8_t v) {
  s_vol = v > 100 ? 100 : v;
  saveSlot("vol", s_vol);
}

uint8_t audioCycleAlert() {
  audioSetAlertMode((uint8_t)((s_alert + 1) % DP_SND_COUNT));
  return s_alert;
}

void audioPlay(uint8_t kind) {
  if (!s_ok || !s_queue) return;
  xQueueSend(s_queue, &kind, 0);  // 队列满了就丢，提示音不值得阻塞主循环
}

void audioPlayEvent(bool complete) {
  audioPlay(complete ? s_done : s_alert);
}

const char* audioModeLabel(uint8_t mode) { return dpSoundName(mode); }
