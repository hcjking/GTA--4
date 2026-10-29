// Remote control for recording demos (only with ?demo=1): polls demo-cmd.json
// on the test server and runs each new command once, so a run can be steered
// without touching the browser window. Commands, by "seq" (run in order once):
//   {"do":"rec","name":"x"}          start recording the game's sound (record.js)
//   {"do":"stop"}                    stop it (pieces in uploads/)
//   {"do":"key","code":"KeyE"}       a key tap (keydown, then keyup 80 ms later)
//   {"do":"hold","vk":87,"ms":8000}  a game key held (Windows virtual key)
// Status (time, last command, recording) goes to uploads/demo-status.json.
(() => {
  if (!/[?&]demo=1\b/.test(location.search)) return;
  let done = 0, last = '';
  const status = extra => fetch('/upload/demo-status.json', { method: 'POST',
    body: JSON.stringify({ at: Date.now(), done, last, recording: window.recording || null,
      visible: document.visibilityState, frames: frames,
      // (Chrome keeps the sound suspended until a real click or key press on the page)
      audio: [...(window.recordContexts || [])].map(c => c.state), ...extra }) }).catch(() => {});
  // (frames the page painted: Chrome stops painting a window it thinks is covered)
  let frames = 0;
  const count = () => { frames++; requestAnimationFrame(count); };
  requestAnimationFrame(count);
  const tap = code => {
    const key = { Enter: 'Enter', Escape: 'Escape' }[code] || code.replace(/^Key/, '').toLowerCase();
    window.dispatchEvent(new KeyboardEvent('keydown', { code, key, bubbles: true }));
    setTimeout(() => window.dispatchEvent(new KeyboardEvent('keyup', { code, key, bubbles: true })), 80);
  };
  const run = async c => {
    if (c.do === 'rec') return window.startRecording(c.name || 'demo', 30, { audioOnly: true });
    if (c.do === 'stop') return await window.stopRecording();
    if (c.do === 'key') return tap(c.code);
    if (c.do === 'hold') {
      Module._rex_web_game_key(c.vk, 1);
      await new Promise(r => setTimeout(r, c.ms || 1000));
      Module._rex_web_game_key(c.vk, 0);
      return 'held';
    }
    return 'unknown';
  };
  setInterval(async () => {
    try {
      const list = await (await fetch('/demo-cmd.json?t=' + Date.now(), { cache: 'no-store' })).json();
      for (const c of list) {
        if (c.seq <= done) continue;
        done = c.seq;
        const result = await run(c);
        last = c.do + ' -> ' + JSON.stringify(result ?? null).slice(0, 200) + ' @' + Date.now();
        status();
      }
    } catch (e) {}
  }, 300);
  setInterval(() => status({ game: (window.bootLog || []).slice(-1)[0] || '' }), 5000);
})();
