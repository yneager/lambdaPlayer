const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const context = {window: {}};
vm.runInNewContext(fs.readFileSync('resources/vui/release-highlights.js', 'utf8'), context);
const summarize = notes => Array.from(context.window.LambdaReleaseHighlights(notes));
test('release dialog shows user highlights without technical detail or markdown links', () => {
  assert.deepEqual(summarize('# New\n- Try **Downloader+**.\n- [Learn more](https://example.com).\n## Details\n- implementation jargon'), ['Try Downloader+.', 'Learn more.']);
  assert.equal(summarize(Array.from({length: 12}, (_, i) => '- Feature ' + i).join('\n')).length, 5);
  assert.equal(summarize('- ' + 'x'.repeat(400))[0].length, 220);
});
test('missing notes have a readable fallback', () => {
  assert.deepEqual(summarize(''), ['Improvements and fixes for a smoother experience.']);
});
