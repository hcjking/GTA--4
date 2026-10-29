// Records what the page shows (the #scene canvas: the game and its overlay)
// with the game's sound, in 10 s pieces uploaded to the test server as
// uploads/<name>-NNNN.webm (each upload must stay under serve.py's 64 MB).
// window.startRecording(name) / window.stopRecording() -> promise of the piece count.
(() => {
  // The sound: everything connected to an AudioContext's speakers also goes to
  // a stream of that context's (set up before SDL makes its context).
  const connect = AudioNode.prototype.connect;
  AudioNode.prototype.connect = function (target, ...rest) {
    const result = connect.call(this, target, ...rest);
    if (target instanceof AudioDestinationNode) {
      const ctx = target.context;
      ctx.recordTap ??= ctx.createMediaStreamDestination();
      try { connect.call(this, ctx.recordTap); } catch (e) {}
      (window.recordContexts ??= new Set()).add(ctx);
    }
    return result;
  };

  let recorder = null, uploads = [], pieces = 0;
  // (opts.audioOnly: just the sound, for a screen recording made outside the page)
  window.startRecording = (name, fps = 30, opts = {}) => {
    if (recorder) return 'already recording';
    const stream = opts.audioOnly ? new MediaStream() : document.getElementById('scene').captureStream(fps);
    for (const ctx of window.recordContexts || [])
      for (const track of ctx.recordTap.stream.getAudioTracks()) stream.addTrack(track);
    const type = (opts.audioOnly ? ['audio/webm;codecs=opus', 'audio/webm']
      : ['video/webm;codecs=vp9,opus', 'video/webm;codecs=vp8,opus', 'video/webm'])
      .find(t => MediaRecorder.isTypeSupported(t));
    recorder = new MediaRecorder(stream, { mimeType: type, videoBitsPerSecond: 6e6, audioBitsPerSecond: 160e3 });
    pieces = 0, uploads = [];
    recorder.ondataavailable = e => {
      if (!e.data.size) return;
      const piece = name + '-' + String(pieces++).padStart(4, '0') + '.webm';
      uploads.push(fetch('/upload/' + piece, { method: 'POST', body: e.data }).then(r => r.ok ? piece : 'failed ' + piece));
    };
    recorder.start(10000);
    window.recording = { name, type, audioTracks: stream.getAudioTracks().length, started: performance.now(), startedAt: Date.now() };
    return window.recording;
  };
  window.stopRecording = () => new Promise(resolve => {
    if (!recorder) return resolve('not recording');
    recorder.onstop = async () => { recorder = null; resolve(await Promise.all(uploads)); };
    recorder.stop();
  });
})();
