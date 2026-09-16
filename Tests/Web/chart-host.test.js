const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const echarts = require(path.resolve(__dirname, '..', '..', 'Resources', 'Web', 'vendor', 'echarts.min.js'));
const templates = require(path.resolve(__dirname, '..', '..', 'Resources', 'Web', 'templates.js'));

const hostSource = fs.readFileSync(
  path.resolve(__dirname, '..', '..', 'Resources', 'Web', 'chart-host.js'),
  'utf8',
);

function createHostContext() {
  const chartElement = {};
  const charts = [];
  const observers = [];
  class FakeResizeObserver {
    constructor(callback) {
      this.callback = callback;
      this.disconnected = false;
      this.observed = null;
      observers.push(this);
    }
    observe(element) { this.observed = element; }
    disconnect() { this.disconnected = true; }
  }
  const window = {
    location: { search: '?generation=7' },
    'echarts-gl': {},
    echarts: {
      init() {
        const chart = {
          disposed: false,
          resizeCount: 0,
          clear() {},
          setOption() {},
          getOption() { return { series: [] }; },
          resize() { this.resizeCount += 1; },
          dispose() { this.disposed = true; },
        };
        charts.push(chart);
        return chart;
      },
    },
    UEEChartsTemplates: {
      detectWebGL() { return true; },
      createTemplate(template) { return { requestedTemplate: template, effectiveTemplate: template, option: {} }; },
      applyInteractionMode(option) { return option; },
    },
  };
  const logs = [];
  const context = vm.createContext({
    Array,
    atob(value) { return Buffer.from(value, 'base64').toString('binary'); },
    ResizeObserver: FakeResizeObserver,
    String,
    TextDecoder,
    Uint8Array,
    URLSearchParams,
    console: { log(value) { logs.push(value); } },
    document: { getElementById() { return chartElement; } },
    window,
  });
  return { chartElement, charts, context, logs, observers, window };
}

function encodePayload(payload) {
  return Buffer.from(JSON.stringify(payload), 'utf8').toString('base64');
}

test('ResizeObserver drives chart resize and dispose releases observer and chart references', () => {
  const state = createHostContext();
  vm.runInContext(hostSource, state.context);

  assert.equal(state.observers.length, 1);
  assert.equal(state.observers[0].observed, state.chartElement);
  state.observers[0].callback();
  assert.equal(state.charts[0].resizeCount, 1);

  const host = state.window.UEEChartsHost;
  host.dispose();
  assert.equal(state.observers[0].disconnected, true);
  assert.equal(state.charts[0].disposed, true);
  assert.equal(state.window.UEEChartsHost, undefined);
  host.resize();
  assert.equal(state.charts[0].resizeCount, 1);
});

test('reinitializing the host disposes the prior chart and observer without timers', () => {
  const state = createHostContext();
  vm.runInContext(hostSource, state.context);
  vm.runInContext(hostSource, state.context);

  assert.equal(state.charts.length, 2);
  assert.equal(state.observers.length, 2);
  assert.equal(state.charts[0].disposed, true);
  assert.equal(state.observers[0].disconnected, true);
  assert.equal(state.observers[1].observed, state.chartElement);
  assert.doesNotMatch(hostSource, /setInterval\s*\(|setTimeout\s*\(/);
});

test('applyDataBase64 maps numeric, category, and 3D series without evaluating data', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.UEEChartsTemplates.createTemplate = (template) => ({
    requestedTemplate: template,
    effectiveTemplate: template,
    option: { tooltip: { trigger: 'axis' }, legend: {}, xAxis: {}, yAxis: {}, series: [{ smooth: true }] },
  });
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');

  const injection = '\"\\\\\nUnicode 数据 😀 </script>;globalThis.__executed=true';
  const payload = {
    revision: 42,
    template: 'SegmentedAreaLine',
    xAxisMode: 'Category',
    series: [
      { index: 0, name: injection, type: 'numeric2D', data: [[1, 2], [3, 4]] },
      { index: 1, name: '类别', type: 'category', data: [['A', 5], ['B', 6]] },
      { index: 2, name: '空间', type: 'data3D', data: [[1, 2, 3, 4, 12]] },
    ],
  };

  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true);
  assert.equal(appliedOption.xAxis.type, 'category');
  assert.deepEqual(Array.from(appliedOption.xAxis.data), ['A', 'B']);
  assert.equal(appliedOption.series[0].type, 'line');
  assert.equal(JSON.stringify(appliedOption.series[0].data), JSON.stringify([[1, 2], [3, 4]]));
  assert.equal(appliedOption.series[0].smooth, true);
  assert.equal(appliedOption.series[0].name, injection);
  assert.equal(appliedOption.series[1].type, 'line');
  assert.deepEqual(Array.from(appliedOption.series[1].data), [5, 6]);
  assert.equal(appliedOption.series[2].type, 'scatter');
  assert.equal(JSON.stringify(appliedOption.series[2].data), JSON.stringify([[1, 2, 3, 4, 12]]));
  assert.equal(state.context.__executed, undefined);
  assert.ok(state.logs.includes('__UE_ECHARTS_APPLIED__:7:42:5'));
});

test('applyDataBase64 rejects malformed Base64 and JSON with an ERROR marker', () => {
  const state = createHostContext();
  vm.runInContext(hostSource, state.context);

  assert.equal(state.window.UEEChartsHost.applyDataBase64('%%%not-base64%%%'), false);
  assert.ok(state.logs.some((line) => line.startsWith('__UE_ECHARTS_ERROR__:7:')));
  assert.doesNotMatch(hostSource, /\beval\s*\(|new\s+Function\s*\(/);
});

test('category series share a deterministic union domain and align missing values with null', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.UEEChartsTemplates.createTemplate = (template) => ({
    requestedTemplate: template,
    effectiveTemplate: template,
    option: { xAxis: {}, yAxis: {}, series: [] },
  });
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');

  const payload = {
    revision: 1,
    template: 'SegmentedAreaLine',
    xAxisMode: 'Category',
    series: [
      { index: 0, name: 'S0', type: 'category', data: [['A', 1], ['B', 2]] },
      { index: 1, name: 'S1', type: 'category', data: [['B', 3], ['A', 4]] },
      { index: 2, name: 'S2', type: 'category', data: [['B', 5]] },
    ],
  };

  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true);
  assert.equal(JSON.stringify(appliedOption.xAxis.data), JSON.stringify(['A', 'B']));
  assert.equal(JSON.stringify(appliedOption.series[0].data), JSON.stringify([1, 2]));
  assert.equal(JSON.stringify(appliedOption.series[1].data), JSON.stringify([4, 3]));
  assert.equal(JSON.stringify(appliedOption.series[2].data), JSON.stringify([null, 5]));
});

test('duplicate category labels use the last value in the same series', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.UEEChartsTemplates.createTemplate = (template) => ({
    requestedTemplate: template,
    effectiveTemplate: template,
    option: { xAxis: {}, yAxis: {}, series: [] },
  });
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');

  const payload = {
    revision: 2,
    template: 'SegmentedAreaLine',
    xAxisMode: 'Category',
    series: [{ index: 0, name: 'S0', type: 'category', data: [['A', 1], ['A', 9], ['B', 2]] }],
  };
  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true);
  assert.equal(JSON.stringify(appliedOption.xAxis.data), JSON.stringify(['A', 'B']));
  assert.equal(JSON.stringify(appliedOption.series[0].data), JSON.stringify([9, 2]));
});

test('3D data maps by the current effective template including WebGL fallbacks', () => {
  const cases = [
    { requested: 'Bar3DHeightMap', effective: 'Bar3DHeightMap', expectedType: 'bar3D', hasGrid3D: true },
    { requested: 'DataTableScatter3D', effective: 'DataTableScatter3D', expectedType: 'scatter3D', hasGrid3D: true },
    { requested: 'Bar3DHeightMap', effective: 'Bar3DHeightMap2D', expectedType: 'heatmap', hasGrid3D: false, heatmap: true },
    { requested: 'DataTableScatter3D', effective: 'DataTableScatter2D', expectedType: 'scatter', hasGrid3D: false },
  ];

  for (const item of cases) {
    const state = createHostContext();
    let appliedOption = null;
    state.window.UEEChartsTemplates.createTemplate = () => ({
      requestedTemplate: item.requested,
      effectiveTemplate: item.effective,
      option: item.hasGrid3D
        ? { grid3D: {}, xAxis3D: {}, yAxis3D: {}, zAxis3D: {}, series: [{ type: item.expectedType }] }
        : { xAxis: {}, yAxis: {}, series: [{ type: item.expectedType }] },
    });
    state.window.echarts.init = () => ({
      clear() {}, resize() {}, dispose() {},
      setOption(option) { appliedOption = option; },
      getOption() { return appliedOption || { series: [] }; },
    });
    vm.runInContext(hostSource, state.context);
    state.window.UEEChartsHost.renderTemplate(item.requested, {}, 'ClickOnly');
    const payload = {
      revision: 3,
      template: item.requested,
      xAxisMode: 'ShowAll',
      series: [{ index: 0, name: '3D', type: 'data3D', data: [[1, 2, 3, 4, 5]] }],
    };

    assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true, item.effective);
    assert.equal(appliedOption.series[0].type, item.expectedType, item.effective);
    assert.equal(JSON.stringify(appliedOption.series[0].data),
      JSON.stringify(item.heatmap ? [[0, 0, 3]] : [[1, 2, 3, 4, 5]]), item.effective);
    if (item.heatmap) {
      assert.equal(JSON.stringify(appliedOption.series[0].ueOriginalData), JSON.stringify([[1, 2, 3, 4, 5]]));
      assert.equal(JSON.stringify(appliedOption.xAxis.data), JSON.stringify([1]));
      assert.equal(JSON.stringify(appliedOption.yAxis.data), JSON.stringify([2]));
    }
    if (!item.hasGrid3D) assert.doesNotMatch(appliedOption.series[0].type, /3D$/);
  }
});

test('Bar3D height-map fallback produces finite heatmap rectangles for arbitrary numeric coordinates', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.location.search = '?generation=7&forceWebGL=0';
  state.window.UEEChartsTemplates = templates;
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  assert.equal(state.window.UEEChartsHost.renderTemplate('Bar3DHeightMap', {}, 'ClickOnly'), 'Bar3DHeightMap2D');

  const payload = {
    revision: 4,
    template: 'Bar3DHeightMap',
    xAxisMode: 'ShowAll',
    series: [{
      index: 0,
      name: 'Heat',
      type: 'data3D',
      data: [[10.5, 200, 3, 30, 5], [-7.25, 900, 6, 60, 8]],
    }],
  };
  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true);

  const chart = echarts.init(null, null, { renderer: 'svg', ssr: true, width: 320, height: 200 });
  try {
    chart.setOption(appliedOption);
    const option = chart.getOption();
    assert.equal(option.xAxis[0].type, 'category');
    assert.equal(option.yAxis[0].type, 'category');
    assert.equal(JSON.stringify(option.xAxis[0].data), JSON.stringify([10.5, -7.25]));
    assert.equal(JSON.stringify(option.yAxis[0].data), JSON.stringify([200, 900]));
    assert.equal(JSON.stringify(option.series[0].data), JSON.stringify([[0, 0, 3], [1, 1, 6]]));
    assert.equal(JSON.stringify(option.series[0].ueOriginalData), JSON.stringify(payload.series[0].data));

    const rects = chart.getZr().storage.getDisplayList(true).filter((item) => item.type === 'rect');
    assert.ok(rects.length >= payload.series[0].data.length);
    const firstHeatmapRect = rects[rects.length - payload.series[0].data.length];
    for (const field of ['x', 'y', 'width', 'height']) {
      assert.equal(Number.isFinite(firstHeatmapRect.shape[field]), true, `heatmap ${field} must be finite`);
    }
  } finally {
    chart.dispose();
  }
});

test('scatter fallback type transitions use a stable template baseline and keep finite nonzero graphics', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.location.search = '?generation=7&forceWebGL=0';
  state.window.UEEChartsTemplates = templates;
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  assert.equal(state.window.UEEChartsHost.renderTemplate('DataTableScatter3D', {}, 'ClickOnly'), 'DataTableScatter2D');

  const apply = (revision, type, data) => state.window.UEEChartsHost.applyDataBase64(encodePayload({
    revision,
    template: 'DataTableScatter3D',
    xAxisMode: 'ShowAll',
    series: [{ index: 0, name: 'S', type, data }],
  }));
  assert.equal(apply(1, 'numeric2D', [[10, 20], [30, 40]]), true);
  assert.equal(apply(2, 'data3D', [[1, 2, 3, 4, 18]]), true);
  assert.equal(apply(3, 'numeric2D', [[10, 20], [30, 40]]), true);

  const chart = echarts.init(null, null, { renderer: 'svg', ssr: true, width: 320, height: 200 });
  try {
    chart.setOption(appliedOption);
    const option = chart.getOption();
    assert.notEqual(typeof option.series[0].symbolSize, 'function');
    assert.equal(option.series[0].dimensions, undefined);
    assert.equal(option.series[0].ueOriginalData, undefined);
    assert.equal(Array.isArray(option.series[0].encode.x) ? option.series[0].encode.x[0] : option.series[0].encode.x, 0);
    assert.equal(Array.isArray(option.series[0].encode.y) ? option.series[0].encode.y[0] : option.series[0].encode.y, 1);

    const graphics = chart.getZr().storage.getDisplayList(true).filter((item) => typeof item.getBoundingRect === 'function');
    const bounds = graphics.map((item) => item.getBoundingRect());
    assert.ok(bounds.some((box) => Number.isFinite(box.width) && Number.isFinite(box.height) && box.width > 0 && box.height > 0));
    assert.ok(bounds.every((box) => ['x', 'y', 'width', 'height'].every((field) => Number.isFinite(box[field]))));
  } finally {
    chart.dispose();
  }
});

test('stable template cloning preserves CustomOption function fields', () => {
  const state = createHostContext();
  let appliedOption = null;
  const formatter = function () { return 'kept'; };
  state.window.UEEChartsTemplates.createTemplate = () => ({
    requestedTemplate: 'CustomOption',
    effectiveTemplate: 'CustomOption',
    option: { tooltip: { formatter }, xAxis: {}, yAxis: {}, series: [{ type: 'line' }] },
  });
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('CustomOption', {}, 'ClickOnly');
  const payload = {
    revision: 1,
    template: 'CustomOption',
    xAxisMode: 'ShowAll',
    series: [{ index: 0, name: 'S', type: 'numeric2D', data: [[1, 2]] }],
  };
  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true);
  assert.equal(appliedOption.tooltip.formatter, formatter);
});

test('native Bar3D data uses value axes for arbitrary XYZ coordinates', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.location.search = '?generation=7&forceWebGL=1';
  state.window.UEEChartsTemplates = templates;
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  assert.equal(state.window.UEEChartsHost.renderTemplate('Bar3DHeightMap', {}, 'ClickOnly'), 'Bar3DHeightMap');
  const payload = {
    revision: 1,
    template: 'Bar3DHeightMap',
    xAxisMode: 'ShowAll',
    series: [{ index: 0, name: '3D', type: 'data3D', data: [[10.5, 200, 3, 4, 5]] }],
  };
  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload(payload)), true);
  assert.equal(appliedOption.series[0].type, 'bar3D');
  assert.equal(appliedOption.xAxis3D.type, 'value');
  assert.equal(appliedOption.yAxis3D.type, 'value');
  assert.equal(appliedOption.zAxis3D.type, 'value');
});
