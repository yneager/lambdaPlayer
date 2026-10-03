// Release authors put short, user-facing bullets before any technical details.
(() => {
  window.LambdaReleaseHighlights = notes => {
    const lines = String(notes || '').split(/\r?\n/);
    const bullets = [];
    for (const line of lines) {
      if (/^#{1,3}\s+(details|technical|checks|validation|developer)/i.test(line)) break;
      if (!/^\s*[-*]\s+/.test(line)) continue;
      const clean = line.replace(/^\s*[-*]\s+/, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1').replace(/[*`]/g, '').trim();
      if (clean) bullets.push(clean.length > 220 ? clean.slice(0, 219) + '…' : clean);
      if (bullets.length === 5) break;
    }
    return bullets.length ? bullets : ['Improvements and fixes for a smoother experience.'];
  };
})();
