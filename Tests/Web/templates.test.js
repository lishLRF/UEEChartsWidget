const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const templatesPath = path.resolve(__dirname, '..', '..', 'Resources', 'Web', 'templates.js');
const templates = require(templatesPath);
const echarts = require(path.resolve(__dirname, '..', '..', 'Resources', 'Web', 'vendor', 'echarts.min.js'));

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

test('recursively applies interaction rules to object and array components without losing responsive fields', () => {
  const series = (name) => ({ name, type: 'line', data: [1, 2] });
  const scope = (name) => ({
    tooltip: [{ trigger: 'item' }, { axisPointer: { type: 'shadow' } }],
    legend: [{ data: [name] }, { left: 12 }],
    grid3D: [{ viewControl: { distance: 220 } }, { boxWidth: 40 }],
    series: series(name),
  });
  const source = {
    ...scope('root'),
    customRootField: { keep: true },
    baseOption: { ...scope('base'), timeline: { data: ['A'] } },
    options: [{ ...scope('timeline'), title: { text: 'Timeline' } }, null],
    media: [{ query: { maxWidth: 600 }, option: { ...scope('media'), dataset: { source: [['x', 'y']] } } }],
  };

  const disabled = templates.applyInteractionMode(source, 'Disabled');
  const scopes = [disabled, disabled.baseOption, disabled.options[0], disabled.media[0].option];
  for (const item of scopes) {
    assert.ok(item.tooltip.every((tooltip) => tooltip.show === false && tooltip.triggerOn === 'none'));
    assert.ok(item.legend.every((legend) => legend.selectedMode === false));
    assert.ok(item.series.every((itemSeries) => itemSeries.silent === true));
    assert.ok(item.grid3D.every((grid) => grid.viewControl.rotateSensitivity === 0));
    assert.ok(item.grid3D.every((grid) => grid.viewControl.zoomSensitivity === 0));
    assert.ok(item.grid3D.every((grid) => grid.viewControl.panSensitivity === 0));
  }
  assert.deepEqual(disabled.media[0].query, { maxWidth: 600 });
  assert.deepEqual(disabled.customRootField, { keep: true });
  assert.deepEqual(disabled.media[0].option.dataset, { source: [['x', 'y']] });
  assert.equal(disabled.options[1], null);

  const click = templates.applyInteractionMode(source, 'ClickOnly');
  assert.ok(click.media[0].option.tooltip.every((tooltip) => tooltip.triggerOn === 'click'));
  assert.ok(click.baseOption.legend.every((legend) => legend.selectedMode === true));
  assert.ok(click.options[0].grid3D.every((grid) => grid.viewControl.zoomSensitivity === 0));

  const hover = templates.applyInteractionMode(source, 'FullHover');
  assert.ok(hover.options[0].tooltip.every((tooltip) => tooltip.triggerOn === 'mousemove|click'));
  assert.ok(hover.media[0].option.grid3D.every((grid) => grid.viewControl.zoomSensitivity > 0));
  assert.ok(hover.media[0].option.series.every((itemSeries) => itemSeries.silent === false));
});

test('bundled ECharts SSR observes Disabled interaction settings after setOption', () => {
  const source = {
    baseOption: {
      timeline: { data: ['Frame'] },
      tooltip: [{ trigger: 'item' }],
      legend: [{ data: ['Base'] }],
      xAxis: { type: 'category', data: ['A', 'B'] },
      yAxis: { type: 'value' },
      series: { name: 'Base', type: 'line', data: [1, 2] },
    },
    options: [{
      tooltip: [{ trigger: 'axis' }],
      legend: [{ data: ['Frame'] }],
      series: { name: 'Frame', type: 'line', data: [2, 1] },
    }],
  };
  const chart = echarts.init(null, null, { renderer: 'svg', ssr: true, width: 320, height: 200 });
  try {
    chart.setOption(templates.applyInteractionMode(source, 'Disabled'));
    const applied = chart.getOption();
    assert.equal(applied.tooltip[0].show, false);
    assert.equal(applied.tooltip[0].triggerOn, 'none');
    assert.equal(applied.legend[0].selectedMode, false);
    assert.ok(applied.series.every((series) => series.silent === true));
  } finally {
    chart.dispose();
  }
});

test('rejects unsafe generated height-map sizes before allocation and accepts the hard boundary', () => {
  assert.deepEqual(templates.limits, { maxHeightMapSide: 128, maxGeneratedPoints: 16384 });
  const originalArrayFrom = Array.from;
  let allocated = false;
  Array.from = function () {
    allocated = true;
    throw new Error('unexpected allocation');
  };
  try {
    assert.throws(
      () => templates.createTemplate('Bar3DHeightMap', { size: 100000, data: [] }, true),
      /size.*1.*128/i,
    );
    assert.equal(allocated, false);
  } finally {
    Array.from = originalArrayFrom;
  }

  const boundary = templates.createTemplate('Bar3DHeightMap', { size: 128 }, true);
  assert.equal(boundary.option.series[0].data.length, 16384);
  const provided = templates.createTemplate('Bar3DHeightMap', { size: 128, data: [] }, true);
  assert.equal(provided.option.series[0].data.length, 0);
  assert.throws(() => templates.createTemplate('Bar3DHeightMap', { size: 129 }, true), /size.*1.*128/i);
  assert.throws(() => templates.createTemplate('Bar3DHeightMap', { size: Number.POSITIVE_INFINITY }, true), /size.*1.*128/i);
});
