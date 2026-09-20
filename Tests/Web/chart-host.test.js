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

test('runtime interaction changes reapply nested component rules without polling', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.UEEChartsTemplates = Object.assign({}, templates);
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || { series: [] }; },
  });
  vm.runInContext(hostSource, state.context);
  const option = {
    tooltip: { triggerOn: 'mousemove' }, legend: {}, series: { type: 'line', data: [1] },
    baseOption: { tooltip: {}, legend: {}, series: [{ type: 'line', data: [2] }] },
    options: [{ tooltip: {}, series: { type: 'bar', data: [3] } }],
    media: [{ option: { tooltip: {}, legend: {}, grid3D: { viewControl: { autoRotate: true } }, series: [{ type: 'bar3D', data: [[0, 0, 1]] }] } }],
  };
  state.window.UEEChartsTemplates.createTemplate = () => ({
    requestedTemplate: 'CustomOption', effectiveTemplate: 'CustomOption', option,
  });
  state.window.UEEChartsHost.renderTemplate('CustomOption', {}, 'ClickOnly');

  assert.equal(state.window.UEEChartsHost.setInteractionMode(11, 'Disabled'), true);
  assert.equal(appliedOption.tooltip.triggerOn, 'none');
  assert.equal(appliedOption.series[0].silent, true);
  assert.equal(appliedOption.baseOption.tooltip.triggerOn, 'none');
  assert.equal(appliedOption.options[0].series[0].silent, true);
  assert.equal(appliedOption.media[0].option.grid3D.viewControl.rotateSensitivity, 0);
  assert.equal(appliedOption.media[0].option.grid3D.viewControl.autoRotate, false);

  assert.equal(state.window.UEEChartsHost.setInteractionMode(12, 'FullHover'), true);
  assert.equal(appliedOption.tooltip.triggerOn, 'mousemove|click');
  assert.equal(appliedOption.legend.selectedMode, true);
  assert.equal(appliedOption.series[0].silent, false);
  assert.equal(appliedOption.media[0].option.grid3D.viewControl.rotateSensitivity, 1);
  assert.equal(appliedOption.media[0].option.grid3D.viewControl.zoomSensitivity, 1);
  assert.equal(appliedOption.media[0].option.grid3D.viewControl.panSensitivity, 1);
  assert.ok(state.logs.includes('__UE_ECHARTS_INTERACTION_RESULT__:7:11:1:Disabled'));
  assert.ok(state.logs.includes('__UE_ECHARTS_INTERACTION_RESULT__:7:12:1:FullHover'));
  assert.doesNotMatch(hostSource, /mousemove[^|]*addEventListener|setInterval\s*\(/);
});

test('custom option Base64 safely applies hostile JSON and reports recoverable setOption errors', () => {
  const state = createHostContext();
  let appliedOption = { title: { text: 'previous' } };
  let failNext = false;
  state.window.UEEChartsTemplates = Object.assign({}, templates);
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) {
      if (failNext) { failNext = false; throw new Error('synthetic setOption failure'); }
      appliedOption = option;
    },
    getOption() { return appliedOption; },
  });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');
  const hostile = '</script> "quotes" \\ slash 数据 😀';
  const option = { title: { text: hostile }, tooltip: {}, legend: {}, series: [{ type: 'line', data: [1, 2] }] };

  assert.equal(state.window.UEEChartsHost.applyOptionBase64(21, encodePayload(option)), true);
  assert.equal(appliedOption.title.text, hostile);
  assert.equal(appliedOption.tooltip.triggerOn, 'click');
  assert.ok(state.logs.includes('__UE_ECHARTS_OPTION_RESULT__:7:21:1:CustomOption applied'));
  assert.equal(state.context.__executed, undefined);

  failNext = true;
  assert.equal(state.window.UEEChartsHost.applyOptionBase64(22, encodePayload({ title: { text: 'bad' } })), false);
  assert.ok(state.logs.some((line) => line.includes('__UE_ECHARTS_OPTION_RESULT__:7:22:0:synthetic setOption failure')));
  assert.equal(state.window.UEEChartsHost.applyOptionBase64(23, encodePayload({ title: { text: 'recovered' } })), true);
  assert.equal(appliedOption.title.text, 'recovered');
});

test('custom option transaction restores the visible chart after setOption mutates then throws', () => {
  const state = createHostContext();
  let visibleOption = null;
  const copy = value => JSON.parse(JSON.stringify(value));
  state.window.UEEChartsTemplates = Object.assign({}, templates);
  state.window.echarts.init = () => ({
    clear() { visibleOption = {}; }, resize() {}, dispose() {},
    setOption(option) {
      visibleOption = copy(option);
      if (option.title && option.title.text === 'attacker-replacement') {
        visibleOption.series = [];
        throw new Error('xAxis "999" not found');
      }
    },
    getOption() { return copy(visibleOption || {}); },
  });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');
  const good = {
    title: { text: 'known-good' }, tooltip: {}, xAxis: {}, yAxis: {},
    series: [{ type: 'line', data: [3, 1, 4] }],
  };
  assert.equal(state.window.UEEChartsHost.applyOptionBase64(41, encodePayload(good)), true);
  const before = state.window.UEEChartsHost.getOptionForTesting();
  const bad = {
    title: { text: 'attacker-replacement' }, xAxis: {}, yAxis: {},
    series: [{ type: 'line', xAxisIndex: 999, data: [9] }],
  };

  assert.equal(state.window.UEEChartsHost.applyOptionBase64(42, encodePayload(bad)), false);
  assert.deepEqual(state.window.UEEChartsHost.getOptionForTesting(), before);
  assert.equal(state.window.UEEChartsHost.getOptionForTesting().series.length, 1);
  assert.ok(state.logs.some(line => line.includes('__UE_ECHARTS_OPTION_RESULT__:7:42:0:')));
});

test('custom option reports a terminal host error when rollback itself fails', () => {
  const state = createHostContext();
  let rollbackMustFail = false;
  state.window.UEEChartsTemplates = Object.assign({}, templates);
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {}, getOption() { return { title: { text: 'known-good' }, series: [] }; },
    setOption(option) {
      if (rollbackMustFail) throw new Error('synthetic rollback failure');
      if (option.title && option.title.text === 'attacker-replacement') {
        rollbackMustFail = true;
        throw new Error('synthetic apply failure');
      }
    },
  });
  vm.runInContext(hostSource, state.context);

  assert.equal(state.window.UEEChartsHost.applyOptionBase64(43, encodePayload({ title: { text: 'attacker-replacement' } })), false);
  assert.ok(state.logs.some(line => line.includes('__UE_ECHARTS_OPTION_RESULT__:7:43:0:') && line.includes('rollback failed')));
  assert.ok(state.logs.some(line => line.startsWith('__UE_ECHARTS_ERROR__:7:option rollback failed:')));
});

test('custom option rejects arrays malformed UTF-8 and decoded payloads above 16 MiB', () => {
  const state = createHostContext();
  vm.runInContext(hostSource, state.context);

  assert.equal(state.window.UEEChartsHost.applyOptionBase64(1, encodePayload([])), false);
  assert.equal(state.window.UEEChartsHost.applyOptionBase64(2, Buffer.from([0xc3, 0x28]).toString('base64')), false);
  const oversized = Buffer.alloc(16 * 1024 * 1024 + 1, 0x20).toString('base64');
  assert.equal(state.window.UEEChartsHost.applyOptionBase64(3, oversized), false);
  assert.equal(state.logs.filter((line) => line.startsWith('__UE_ECHARTS_OPTION_RESULT__:7:')).length, 3);
});

test('advanced JavaScript receives only chart echarts and host, ACKs success, and recovers after errors', () => {
  const state = createHostContext();
  let appliedOption = null;
  state.window.echarts.init = () => ({
    clear() {}, resize() {}, dispose() {},
    setOption(option) { appliedOption = option; },
    getOption() { return appliedOption || {}; },
  });
  vm.runInContext(hostSource, state.context);
  const modify = Buffer.from("chart.setOption({title:{text:'raw 数据 😀'}}); if (!echarts || !host) throw new Error('missing API');", 'utf8').toString('base64');
  assert.equal(state.window.UEEChartsHost.executeJavaScriptBase64(31, modify), true);
  assert.equal(appliedOption.title.text, 'raw 数据 😀');
  assert.ok(state.logs.includes('__UE_ECHARTS_JAVASCRIPT_RESULT__:7:31:1:JavaScript executed'));

  const throwing = Buffer.from("throw new Error('raw failure')", 'utf8').toString('base64');
  assert.equal(state.window.UEEChartsHost.executeJavaScriptBase64(32, throwing), false);
  assert.ok(state.logs.some((line) => line.includes('__UE_ECHARTS_JAVASCRIPT_RESULT__:7:32:0:raw failure')));
  assert.equal(state.window.UEEChartsHost.executeJavaScriptBase64(33, Buffer.from('chart.resize()', 'utf8').toString('base64')), true);
  assert.ok(state.logs.includes('__UE_ECHARTS_JAVASCRIPT_RESULT__:7:33:1:JavaScript executed'));
  assert.doesNotMatch(hostSource, /UObject|ue\.interface|window\.ue/);
});

test('advanced JavaScript rejects empty malformed UTF-8 and decoded payloads above 1 MiB', () => {
  const state = createHostContext();
  vm.runInContext(hostSource, state.context);

  assert.equal(state.window.UEEChartsHost.executeJavaScriptBase64(1, ''), false);
  assert.equal(state.window.UEEChartsHost.executeJavaScriptBase64(2, Buffer.from([0xc3, 0x28]).toString('base64')), false);
  const oversized = Buffer.alloc(1024 * 1024 + 1, 0x20).toString('base64');
  assert.equal(state.window.UEEChartsHost.executeJavaScriptBase64(3, oversized), false);
  assert.equal(state.logs.filter((line) => line.startsWith('__UE_ECHARTS_JAVASCRIPT_RESULT__:7:')).length, 3);
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
  assert.doesNotMatch(hostSource, /\beval\s*\(/);
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

test('stream category windows retain repeated X labels in append order across looping replacements', () => {
  const state = createHostContext();
  let option;
  state.window.UEEChartsTemplates.createTemplate = (template) => ({
    requestedTemplate: template, effectiveTemplate: template, option: { xAxis: {}, yAxis: {}, series: [] },
  });
  state.window.echarts.init = () => ({ clear() {}, resize() {}, dispose() {}, setOption(value) { option = value; } });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');
  for (let revision = 1; revision <= 100; revision += 1) {
    const data = revision % 2 ? [['A', 1], ['B', 2], ['A', 3]] : [['B', 2], ['A', 3], ['B', 4]];
    assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload({
      revision, template: 'SegmentedAreaLine', xAxisMode: 'Category', preserveCategoryOrder: true,
      series: [{ index: 0, name: 'Stream', type: 'category', data }],
    })), true);
    assert.equal(JSON.stringify(option.xAxis.data), JSON.stringify(data.map(point => point[0])));
    assert.equal(JSON.stringify(option.series[0].data), JSON.stringify(data.map(point => point[1])));
    assert.equal(option.series[0].data.length, 3);
  }
});

test('stream category order appends labels unique to other series without collapsing repeated slots', () => {
  const state = createHostContext();
  let option;
  state.window.UEEChartsTemplates.createTemplate = (template) => ({
    requestedTemplate: template, effectiveTemplate: template, option: { xAxis: {}, yAxis: {}, series: [] },
  });
  state.window.echarts.init = () => ({ clear() {}, resize() {}, dispose() {}, setOption(value) { option = value; } });
  vm.runInContext(hostSource, state.context);
  state.window.UEEChartsHost.renderTemplate('SegmentedAreaLine', {}, 'ClickOnly');
  assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload({
    revision: 1, template: 'SegmentedAreaLine', xAxisMode: 'Category', preserveCategoryOrder: true,
    series: [
      { index: 0, name: 'Stream', type: 'category', data: [['A', 1], ['B', 2], ['A', 3]] },
      { index: 1, name: 'Reference', type: 'category', data: [['B', 9], ['C', 4]] },
    ],
  })), true);
  assert.equal(JSON.stringify(option.xAxis.data), JSON.stringify(['A', 'B', 'A', 'C']));
  assert.equal(JSON.stringify(option.series[0].data), JSON.stringify([1, 2, 3, null]));
  assert.equal(JSON.stringify(option.series[1].data), JSON.stringify([null, 9, null, 4]));
});

test('stream deltas append and drop numeric category and 3D windows from the retained raw payload', () => {
  const cases = [
    { type: 'numeric2D', initial: [[1, 10], [2, 20]], append: [[3, 30]], expected: [[2, 20], [3, 30]] },
    { type: 'category', initial: [['A', 1], ['B', 2]], append: [['A', 3]], expected: [2, 3], axis: ['B', 'A'] },
    { type: 'data3D', initial: [[1, 2, 3, 4, 5], [6, 7, 8, 9, 10]], append: [[11, 12, 13, 14, 15]], expected: [[6, 7, 8, 9, 10], [11, 12, 13, 14, 15]] },
  ];
  cases.forEach(({ type, initial, append, expected, axis }) => {
    const state = createHostContext();
    let option;
    state.window.UEEChartsTemplates.createTemplate = (template) => ({
      requestedTemplate: template, effectiveTemplate: template, option: { xAxis: {}, yAxis: {}, series: [] },
    });
    state.window.echarts.init = () => ({ clear() {}, resize() {}, dispose() {}, setOption(value) { option = value; } });
    vm.runInContext(hostSource, state.context);
    state.window.UEEChartsHost.renderTemplate(type === 'data3D' ? 'DataTableScatter3D' : 'SegmentedAreaLine', {}, 'ClickOnly');
    assert.equal(state.window.UEEChartsHost.applyDataBase64(encodePayload({
      revision: 1, template: type === 'data3D' ? 'DataTableScatter3D' : 'SegmentedAreaLine',
      xAxisMode: type === 'category' ? 'Category' : 'ShowAll', preserveCategoryOrder: type === 'category',
      series: [{ index: 0, name: 'Stream', type, data: initial }],
    })), true);
    assert.equal(state.window.UEEChartsHost.applyStreamDeltaBase64(encodePayload({
      revision: 2, baseRevision: 1, drop: 1, series: { index: 0, type, data: append },
    })), true);
    assert.equal(JSON.stringify(option.series[0].data), JSON.stringify(expected));
    if (axis) assert.equal(JSON.stringify(option.xAxis.data), JSON.stringify(axis));
  });
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
