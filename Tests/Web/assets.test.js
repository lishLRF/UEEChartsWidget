const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const pluginRoot = path.resolve(__dirname, '..', '..');
const webRoot = path.join(pluginRoot, 'Resources', 'Web');
const vendorRoot = path.join(webRoot, 'vendor');

function read(relativePath) {
  return fs.readFileSync(path.join(pluginRoot, relativePath), 'utf8');
}

function sha256(filePath) {
  return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex');
}

test('bundles pinned official ECharts distributions with verified metadata', () => {
  const manifest = JSON.parse(read('Resources/Web/vendor/manifest.json'));
  const expected = {
    echarts: { version: '6.1.0', license: 'Apache-2.0', file: 'echarts.min.js' },
    'echarts-gl': { version: '2.1.0', license: 'BSD-3-Clause', file: 'echarts-gl.min.js' },
  };

  assert.deepEqual(Object.keys(manifest.packages).sort(), Object.keys(expected).sort());
  for (const [name, pinned] of Object.entries(expected)) {
    const item = manifest.packages[name];
    assert.equal(item.version, pinned.version);
    assert.equal(item.license, pinned.license);
    assert.equal(item.file, pinned.file);
    assert.match(item.source, /^https:\/\/registry\.npmjs\.org\//);
    assert.match(item.sha256, /^[a-f0-9]{64}$/);
    assert.equal(item.sha256, sha256(path.join(vendorRoot, item.file)));
  }
});

test('ships vendor license texts and notices without package-manager residue', () => {
  const notices = read('THIRD_PARTY_NOTICES.md');
  const attributes = read('.gitattributes');
  assert.match(notices, /Apache ECharts 6\.1\.0/);
  assert.match(notices, /echarts-gl 2\.1\.0/);
  assert.ok(fs.existsSync(path.join(pluginRoot, 'ThirdPartyLicenses', 'Apache-ECharts-6.1.0.txt')));
  assert.ok(fs.existsSync(path.join(pluginRoot, 'ThirdPartyLicenses', 'echarts-gl-2.1.0.txt')));
  assert.equal(fs.existsSync(path.join(pluginRoot, 'node_modules')), false);
  assert.equal(fs.existsSync(path.join(pluginRoot, 'package-lock.json')), false);
  assert.equal(fs.readdirSync(vendorRoot).some((name) => /\.(tgz|tar|zip)$/i.test(name)), false);
  assert.match(attributes, /Resources\/Web\/vendor\/\*\.js\s+binary/);
});

test('host page is local-only and loads scripts in strict dependency order', () => {
  const html = read('Resources/Web/chart-host.html');
  const hostRuntime = read('Resources/Web/chart-host.js');
  const sources = [...html.matchAll(/<script\s+src="([^"]+)"\s*><\/script>/g)].map((match) => match[1]);
  assert.deepEqual(sources, [
    'vendor/echarts.min.js',
    'vendor/echarts-gl.min.js',
    'templates.js',
    'chart-host.js',
  ]);
  assert.match(html, /script-src 'self'/);
  assert.match(html, /connect-src 'none'/);
  assert.doesNotMatch(html, /unsafe-inline[^;]*script|script-src[^;]*unsafe-inline/i);
  assert.doesNotMatch(html, /<script(?!\s+src=)[^>]*>/i);
  assert.doesNotMatch(html, /https?:\/\/|src="\/\//i);
  assert.match(html, /html, body, #chart\s*\{[^}]*width:\s*100%[^}]*height:\s*100%[^}]*background:\s*transparent/s);
  assert.match(hostRuntime, /window\['echarts-gl'\]/);
  assert.match(hostRuntime, /echarts-gl vendor script unavailable/);
  assert.doesNotMatch(hostRuntime, /setInterval\s*\(|setTimeout\s*\(/);
});
