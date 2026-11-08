// --mock：不读真实数据，产出一份随时间变化的假状态，
// 用于固件开发时验证三种卡片布局、状态色和断连恢复。
export function mockState(startedAt) {
  const t = Date.now();
  const phase = Math.floor((t / 6000) % 4);
  const states = ['thinking', 'tool', 'done', 'idle'];
  const names = ['WorkBuddy', 'DeepSeek', 'Kimi'];
  const ids = ['workbuddy', 'deepseek', 'kimi'];

  const providers = ids.map((id, i) => {
    const state = states[(phase + i) % states.length];
    const isBalance = id !== 'workbuddy';
    return {
      id,
      name: names[i],
      state,
      state_since: t - (i + 1) * 47000,
      task: {
        title: ['把固件刷进开发板', '校准 240x240 排版', '核对余额口径'][i],
        step: state === 'tool' ? 7 : state === 'thinking' ? 3 : null,
        total: null,
        tool: state === 'tool' ? ['bash', 'write_file', 'read_file'][i] : null,
      },
      context: isBalance ? undefined : { used: 59633, size: 300000 },
      quota: isBalance
        ? {
            kind: 'balance',
            remaining: [0, 87.35, 49.59][i],
            currency: 'CNY',
            spent_today: [0, 2.61, 0][i],
            burn_days_left: [null, 33, null][i],
          }
        : { kind: 'credit', used: 12.4, budget: 100, unit: 'credit' },
      health: i === 2 && state === 'idle' ? 'warn' : 'ok',
      source: 'mock',
    };
  });

  return {
    ts: t,
    bridge: { ver: '0.1.4', uptime_s: Math.round((t - startedAt) / 1000), host: 'mock' },
    providers,
  };
}
