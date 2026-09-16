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
  const context = vm.createContext({
    Array,
    ResizeObserver: FakeResizeObserver,
    String,
    URLSearchParams,
    console: { log() {} },
    document: { getElementById() { return chartElement; } },
    window,
  });
  return { chartElement, charts, context, observers, window };
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
