const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const templatesPath = path.resolve(__dirname, '..', '..', 'Resources', 'Web', 'templates.js');
const templates = require(templatesPath);

test('maps 3D templates to deterministic WebGL and 2D variants', () => {
  assert.equal(templates.selectEffectiveTemplate('SegmentedAreaLine', false), 'SegmentedAreaLine');
  assert.equal(templates.selectEffectiveTemplate('Bar3DHeightMap', true), 'Bar3DHeightMap');
  assert.equal(templates.selectEffectiveTemplate('Bar3DHeightMap', false), 'Bar3DHeightMap2D');
  assert.equal(templates.selectEffectiveTemplate('DataTableScatter3D', false), 'DataTableScatter2D');

  const first = templates.createTemplate('Bar3DHeightMap', {}, true);
  const second = templates.createTemplate('Bar3DHeightMap', {}, true);
  assert.deepEqual(first, second);
  assert.equal(first.option.animation, false);
  assert.equal(first.option.series[0].type, 'bar3D');
  assert.equal(first.option.grid3D.viewControl.autoRotate, false);

  const fallback = templates.createTemplate('Bar3DHeightMap', {}, false);
  assert.equal(fallback.effectiveTemplate, 'Bar3DHeightMap2D');
  assert.equal(fallback.option.series[0].type, 'heatmap');
});

test('builds segmented area, scatter3D fallback, and custom option without network dependencies', () => {
  const segmented = templates.createTemplate('SegmentedAreaLine', {}, false);
  assert.equal(segmented.effectiveTemplate, 'SegmentedAreaLine');
  assert.equal(segmented.option.animation, false);
  assert.equal(segmented.option.series[0].type, 'line');
  assert.ok(segmented.option.series[0].markArea.data.length > 0);

  const scatter = templates.createTemplate('DataTableScatter3D', {}, true);
  assert.equal(scatter.option.series[0].type, 'scatter3D');
  assert.equal(scatter.option.grid3D.viewControl.autoRotate, false);
  const scatterFallback = templates.createTemplate('DataTableScatter3D', {}, false);
  assert.equal(scatterFallback.effectiveTemplate, 'DataTableScatter2D');
  assert.equal(scatterFallback.option.series[0].type, 'scatter');

  const singleSeries = { name: 'Only', type: 'line', data: [3, 1, 4], smooth: true };
  const custom = templates.createTemplate('CustomOption', { option: { title: { text: 'Offline' }, series: singleSeries } }, false);
  assert.equal(custom.option.title.text, 'Offline');
  assert.equal(custom.option.animation, false);
  const interactiveCustom = templates.applyInteractionMode(custom.option, 'ClickOnly');
  assert.ok(Array.isArray(interactiveCustom.series));
  assert.equal(interactiveCustom.series.length, 1);
  assert.deepEqual(interactiveCustom.series[0], { ...singleSeries, silent: false });

  const source = fs.readFileSync(templatesPath, 'utf8');
  assert.doesNotMatch(source, /https?:\/\/|fetch\s*\(|XMLHttpRequest|jquery|simplex-noise/i);
});

test('applies Disabled, ClickOnly, and FullHover interaction rules', () => {
  const base = templates.createTemplate('Bar3DHeightMap', {}, true).option;

  const disabled = templates.applyInteractionMode(base, 'Disabled');
  assert.equal(disabled.tooltip.show, false);
  assert.equal(disabled.tooltip.triggerOn, 'none');
  assert.equal(disabled.legend.selectedMode, false);
  assert.ok(disabled.series.every((series) => series.silent === true));
  assert.equal(disabled.grid3D.viewControl.rotateSensitivity, 0);
  assert.equal(disabled.grid3D.viewControl.zoomSensitivity, 0);
  assert.equal(disabled.grid3D.viewControl.panSensitivity, 0);

  const clickOnly = templates.applyInteractionMode(base, 'ClickOnly');
  assert.equal(clickOnly.tooltip.triggerOn, 'click');
  assert.equal(clickOnly.legend.selectedMode, true);
  assert.equal(clickOnly.grid3D.viewControl.rotateSensitivity, 1);
  assert.equal(clickOnly.grid3D.viewControl.zoomSensitivity, 0);
  assert.equal(clickOnly.grid3D.viewControl.panSensitivity, 0);
  assert.equal(clickOnly.grid3D.viewControl.autoRotate, false);

  const hover = templates.applyInteractionMode(base, 'FullHover');
  assert.equal(hover.tooltip.triggerOn, 'mousemove|click');
  assert.equal(hover.legend.selectedMode, true);
  assert.ok(hover.grid3D.viewControl.rotateSensitivity > 0);
  assert.ok(hover.grid3D.viewControl.zoomSensitivity > 0);
  assert.equal(hover.grid3D.viewControl.autoRotate, false);
});
