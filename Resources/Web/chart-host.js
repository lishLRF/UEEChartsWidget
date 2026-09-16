(function () {
  'use strict';
  const parameters = new URLSearchParams(window.location.search);
  const generation = parameters.get('generation') || '0';

  function emit(kind, payload) {
    console.log('__UE_ECHARTS_' + kind + '__:' + generation + (payload ? ':' + payload : ''));
  }

  function resolveWebGL() {
    const forced = parameters.get('forceWebGL');
    if (forced === '0') return false;
    if (forced === '1') return true;
    return window.UEEChartsTemplates.detectWebGL(document);
  }

  try {
    if (!window.echarts || typeof window.echarts.init !== 'function') throw new Error('Apache ECharts vendor script unavailable');
    if (!window['echarts-gl']) throw new Error('echarts-gl vendor script unavailable');
    if (!window.UEEChartsTemplates || typeof window.UEEChartsTemplates.createTemplate !== 'function') throw new Error('ECharts template runtime unavailable');
    const chart = window.echarts.init(document.getElementById('chart'), null, { renderer: 'canvas' });
    const webglAvailable = resolveWebGL();
    window.UEEChartsHost = {
      renderTemplate: function (template, payload, interactionMode) {
        try {
          const result = window.UEEChartsTemplates.createTemplate(template, payload || {}, webglAvailable);
          const option = window.UEEChartsTemplates.applyInteractionMode(result.option, interactionMode || 'ClickOnly');
          if (result.effectiveTemplate !== result.requestedTemplate) {
            emit('WARNING', 'WebGL unavailable; using ' + result.effectiveTemplate + ' for ' + result.requestedTemplate);
          }
          chart.clear();
          chart.setOption(option, { notMerge: true, lazyUpdate: false });
          emit('RENDERED', result.requestedTemplate + ':' + result.effectiveTemplate);
          return result.effectiveTemplate;
        } catch (error) {
          const detail = error && error.stack ? error.stack.replace(/[\r\n]+/g, ' | ') : (error && error.message ? error.message : String(error));
          emit('ERROR', template + ' render failed: ' + detail);
          return null;
        }
      },
      resize: function () { chart.resize(); },
      dispose: function () { chart.dispose(); }
    };
    emit('READY');
  } catch (error) {
    emit('ERROR', error && error.message ? error.message : String(error));
  }
}());
