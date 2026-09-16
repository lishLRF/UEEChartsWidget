const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

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
    { requested: 'Bar3DHeightMap', effective: 'Bar3DHeightMap2D', expectedType: 'heatmap', hasGrid3D: false },
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
    assert.equal(JSON.stringify(appliedOption.series[0].data), JSON.stringify([[1, 2, 3, 4, 5]]), item.effective);
    if (!item.hasGrid3D) assert.doesNotMatch(appliedOption.series[0].type, /3D$/);
  }
});
