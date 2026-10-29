ObjC.import('AppKit');
// One keyboard prompt picture: a light keycap filling the square (the game
// draws these at text height, ~13 px, so everything is as big as it fits),
// with the key's name in a condensed bold face sized to the cap, or for the
// long names an icon: SPACE a space bar, SHIFT an up arrow, LMB / RMB / MOUSE
// a mouse (the button filled), WASD the four-key cluster.
function run(argv) {
  const out = argv[0], label = argv[1], size = 128;
  const rep = $.NSBitmapImageRep.alloc.initWithBitmapDataPlanesPixelsWidePixelsHighBitsPerSampleSamplesPerPixelHasAlphaIsPlanarColorSpaceNameBytesPerRowBitsPerPixel(null, size, size, 8, 4, true, false, $.NSDeviceRGBColorSpace, 0, 0);
  const ctx = $.NSGraphicsContext.graphicsContextWithBitmapImageRep(rep);
  $.NSGraphicsContext.saveGraphicsState; $.NSGraphicsContext.setCurrentContext(ctx);
  const light = $.NSColor.colorWithDeviceRedGreenBlueAlpha(0.95, 0.95, 0.95, 1);
  const dark = $.NSColor.colorWithDeviceRedGreenBlueAlpha(0.08, 0.08, 0.08, 1);
  const cap = (x, y, w, h, r) => {
    const p = $.NSBezierPath.bezierPathWithRoundedRectXRadiusYRadius($.NSMakeRect(x, y, w, h), r, r);
    light.setFill; p.fill; dark.setStroke; p.setLineWidth(5); p.stroke;
  };
  const text = (s, cx, cy, maxW, maxH) => {
    let fontSize = maxH, attrs, ts;
    for (;; fontSize -= 2) {
      const font = $.NSFont.fontWithNameSize('HelveticaNeue-CondensedBlack', fontSize) ||
                   $.NSFont.boldSystemFontOfSize(fontSize);
      attrs = $.NSDictionary.dictionaryWithObjectsForKeys($([font, dark]), $([$.NSFontAttributeName, $.NSForegroundColorAttributeName]));
      ts = $(s).sizeWithAttributes(attrs);
      if ((ts.width <= maxW && ts.height * 0.8 <= maxH) || fontSize <= 10) break;
    }
    $(s).drawAtPointWithAttributes($.NSMakePoint(cx - ts.width / 2, cy - ts.height / 2), attrs);
  };
  const fill = (points) => {
    const p = $.NSBezierPath.bezierPath;
    p.moveToPoint($.NSMakePoint(points[0][0], points[0][1]));
    for (const [x, y] of points.slice(1)) p.lineToPoint($.NSMakePoint(x, y));
    p.closePath; dark.setFill; p.fill;
  };
  const mouse = (button) => {  // (0 none, 1 left, 2 right)
    const body = $.NSBezierPath.bezierPathWithRoundedRectXRadiusYRadius($.NSMakeRect(30, 6, 68, 116), 34, 34);
    light.setFill; body.fill; dark.setStroke; body.setLineWidth(7); body.stroke;
    if (button) {
      const x0 = button === 1 ? 30 : 64;
      $.NSGraphicsContext.saveGraphicsState;
      $.NSBezierPath.bezierPathWithRoundedRectXRadiusYRadius($.NSMakeRect(30, 6, 68, 116), 34, 34).addClip;
      dark.setFill; $.NSBezierPath.fillRect($.NSMakeRect(x0, 64, 34, 58));
      $.NSGraphicsContext.restoreGraphicsState;
    }
    const line = $.NSBezierPath.bezierPath;
    line.moveToPoint($.NSMakePoint(30, 64)); line.lineToPoint($.NSMakePoint(98, 64));
    line.moveToPoint($.NSMakePoint(64, 64)); line.lineToPoint($.NSMakePoint(64, 122));
    line.setLineWidth(6); dark.setStroke; line.stroke;
  };
  if (label === 'LMB' || label === 'RMB' || label === 'MOUSE') {
    mouse(label === 'LMB' ? 1 : label === 'RMB' ? 2 : 0);
  } else if (label === 'WASD') {
    cap(41, 66, 46, 56, 10); text('W', 64, 94, 38, 46);
    cap(2, 6, 41, 56, 10); text('A', 22.5, 34, 33, 46);
    cap(43.5, 6, 41, 56, 10); text('S', 64, 34, 33, 46);
    cap(85, 6, 41, 56, 10); text('D', 105.5, 34, 33, 46);
  } else {
    cap(4, 6, size - 8, size - 12, 18);
    if (label === 'SPACE') {
      fill([[24, 70], [34, 70], [34, 46], [94, 46], [94, 70], [104, 70], [104, 36], [24, 36]]);
    } else if (label === 'SHIFT') {
      fill([[64, 104], [100, 64], [80, 64], [80, 28], [48, 28], [48, 64], [28, 64]]);
    } else {
      text(label, 64, 64, size - 26, label.length <= 1 ? 84 : 64);
    }
  }
  $.NSGraphicsContext.restoreGraphicsState;
  rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically(out, true);
  return 'ok';
}
