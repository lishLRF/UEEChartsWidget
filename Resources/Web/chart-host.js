(function () {
  'use strict';
  const parameters = new URLSearchParams(window.location.search);
  const generation = parameters.get('generation') || '0';

  function emit(kind, payload) {
    console.log('__UE_ECHARTS_' + kind + '__:' + generation + (payload ? ':' + payload : ''));
  }

  function emitResult(kind, requestId, success, message) {
    const detail = String(message || '').replace(/[\r\n]+/g, ' | ');
    emit(kind, String(requestId) + ':' + (success ? '1' : '0') + ':' + detail);
  }

  function resolveWebGL() {
    const forced = parameters.get('forceWebGL');
    if (forced === '0') return false;
    if (forced === '1') return true;
    return window.UEEChartsTemplates.detectWebGL(document);
  }

  if (window.UEEChartsHost && typeof window.UEEChartsHost.dispose === 'function') {
    window.UEEChartsHost.dispose();
  }

  try {
    if (!window.echarts || typeof window.echarts.init !== 'function') throw new Error('Apache ECharts vendor script unavailable');
    if (!window['echarts-gl']) throw new Error('echarts-gl vendor script unavailable');
    if (!window.UEEChartsTemplates || typeof window.UEEChartsTemplates.createTemplate !== 'function') throw new Error('ECharts template runtime unavailable');
    const chartElement = document.getElementById('chart');
    let chart = window.echarts.init(chartElement, null, { renderer: 'canvas' });
    let resizeObserver = null;
    const webglAvailable = resolveWebGL();
    let currentEffectiveTemplate = null;
    let templateBaseOption = null;
    let currentOption = null;
    let currentPayload = null;
    let currentInteractionMode = 'ClickOnly';

    function clone(value) {
      if (Array.isArray(value)) return value.map(clone);
      if (value && typeof value === 'object') {
        const result = {};
        Object.keys(value).forEach(function (key) { result[key] = clone(value[key]); });
        return result;
      }
      return value;
    }

    function decodeBase64Text(base64, maxBytes, label) {
      if (typeof base64 !== 'string' || base64.length === 0 || base64.length % 4 !== 0 ||
          base64.length > 4 * Math.ceil(maxBytes / 3) || !/^[A-Za-z0-9+/]*={0,2}$/.test(base64)) {
        throw new Error('Invalid Base64 ' + label);
      }
      const binary = atob(base64);
      if (binary.length > maxBytes) throw new Error('Decoded ' + label + ' exceeds ' + maxBytes + ' bytes');
      const bytes = new Uint8Array(binary.length);
      for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
      return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    }

    function decodePayload(base64) {
      return JSON.parse(decodeBase64Text(base64, 16 * 1024 * 1024, 'data payload'));
    }

    function validRequestId(requestId) {
      return Number.isSafeInteger(requestId) && requestId > 0;
    }

    function validatePayload(payload) {
      if (!payload || typeof payload !== 'object' || !Number.isSafeInteger(payload.revision) || payload.revision <= 0) {
        throw new Error('Data payload revision must be a positive integer');
      }
      if (typeof payload.template !== 'string' ||
          !['FollowLatestWindow', 'ShowAll', 'Category'].includes(payload.xAxisMode)) {
        throw new Error('Invalid template or xAxisMode in data payload');
      }
      if (!Array.isArray(payload.series) || payload.series.length > 4) throw new Error('Data payload supports at most 4 series');
      let pointCount = 0;
      payload.series.forEach(function (series, expectedIndex) {
        if (!series || series.index !== expectedIndex || typeof series.name !== 'string' ||
            !['numeric2D', 'category', 'data3D'].includes(series.type) || !Array.isArray(series.data)) {
          throw new Error('Invalid series payload at index ' + expectedIndex);
        }
        const width = series.type === 'numeric2D' ? 2 : (series.type === 'category' ? 2 : 5);
        series.data.forEach(function (point) {
          if (!Array.isArray(point) || point.length !== width) throw new Error('Invalid point width in series ' + expectedIndex);
          if (series.type === 'category') {
            if (typeof point[0] !== 'string' || point[0].trim().length === 0 || !Number.isFinite(point[1])) {
              throw new Error('Invalid category point in series ' + expectedIndex);
            }
          } else if (!point.every(Number.isFinite)) {
            throw new Error('Non-finite numeric point in series ' + expectedIndex);
          }
        });
        pointCount += series.data.length;
      });
      if (pointCount > 100000) throw new Error('Data payload exceeds 100000 points');
      return pointCount;
    }

    function updateAxis(option, key, values) {
      const axes = Array.isArray(option[key]) ? option[key] : [option[key] || {}];
      axes[0] = Object.assign({}, axes[0], values);
      option[key] = Array.isArray(option[key]) ? axes : axes[0];
    }

    function optionForPayload(payload) {
      if (!templateBaseOption || !currentEffectiveTemplate) throw new Error('Template must be rendered before applying data');
      const option = clone(templateBaseOption);
      const oldSeries = Array.isArray(option.series) ? option.series : (option.series ? [option.series] : []);
      const categoryLabels = [];
      const categoryLabelSet = new Set();
      const orderedCategoryWindow = payload.preserveCategoryOrder === true &&
        payload.series.length > 0 && payload.series[0].type === 'category';
      if (orderedCategoryWindow) {
        payload.series[0].data.forEach(function (point) {
          categoryLabels.push(point[0]);
          categoryLabelSet.add(point[0]);
        });
        payload.series.slice(1).forEach(function (series) {
          if (series.type !== 'category') return;
          series.data.forEach(function (point) {
            if (!categoryLabelSet.has(point[0])) {
              categoryLabelSet.add(point[0]);
              categoryLabels.push(point[0]);
            }
          });
        });
      } else {
        payload.series.forEach(function (series) {
          if (series.type !== 'category') return;
          series.data.forEach(function (point) {
            if (!categoryLabelSet.has(point[0])) {
              categoryLabelSet.add(point[0]);
              categoryLabels.push(point[0]);
            }
          });
        });
      }
      const useHeatmapAxes = currentEffectiveTemplate === 'Bar3DHeightMap2D' &&
        payload.series.some(function (series) { return series.type === 'data3D'; });
      const heatmapXDomain = [];
      const heatmapYDomain = [];
      const heatmapXIndices = new Map();
      const heatmapYIndices = new Map();
      if (useHeatmapAxes) {
        payload.series.forEach(function (series) {
          if (series.type !== 'data3D') return;
          series.data.forEach(function (point) {
            if (!heatmapXIndices.has(point[0])) {
              heatmapXIndices.set(point[0], heatmapXDomain.length);
              heatmapXDomain.push(point[0]);
            }
            if (!heatmapYIndices.has(point[1])) {
              heatmapYIndices.set(point[1], heatmapYDomain.length);
              heatmapYDomain.push(point[1]);
            }
          });
        });
      }
      let has2DSeries = false;
      option.series = payload.series.map(function (input, index) {
        const prototype = oldSeries[index] || oldSeries[0] || {};
        const output = Object.assign({}, clone(prototype), { name: input.name });
        delete output.data;
        delete output.dimensions;
        delete output.encode;
        delete output.ueOriginalData;
        if (currentEffectiveTemplate !== 'CustomOption' && typeof output.symbolSize === 'function') {
          delete output.symbolSize;
        }
        if (input.type === 'category') {
          has2DSeries = true;
          const valuesByLabel = new Map();
          input.data.forEach(function (point) { valuesByLabel.set(point[0], point[1]); });
          output.type = 'line';
          output.data = orderedCategoryWindow && index === 0
            ? input.data.map(function (point) { return point[1]; })
              .concat(new Array(categoryLabels.length - input.data.length).fill(null))
            : categoryLabels.map(function (label) {
            return valuesByLabel.has(label) ? valuesByLabel.get(label) : null;
          });
        } else if (input.type === 'data3D') {
          const hasGrid3D = !!option.grid3D;
          if (currentEffectiveTemplate === 'Bar3DHeightMap' && hasGrid3D) {
            output.type = 'bar3D';
          } else if (currentEffectiveTemplate === 'DataTableScatter3D' && hasGrid3D) {
            output.type = 'scatter3D';
          } else if (currentEffectiveTemplate === 'Bar3DHeightMap2D') {
            output.type = 'heatmap';
            has2DSeries = true;
          } else {
            output.type = 'scatter';
            has2DSeries = true;
          }
          output.data = output.type === 'heatmap'
            ? input.data.map(function (point) {
              return [heatmapXIndices.get(point[0]), heatmapYIndices.get(point[1]), point[2]];
            })
            : clone(input.data);
          if (output.type === 'heatmap') {
            output.dimensions = ['xIndex', 'yIndex', 'z'];
            output.ueOriginalData = clone(input.data);
          }
          if (output.type === 'bar3D' || output.type === 'scatter3D') {
            output.encode = { x: 0, y: 1, z: 2, tooltip: [0, 1, 2, 3, 4] };
          } else {
            output.encode = { x: 0, y: 1, value: 2, tooltip: [0, 1, 2, 3, 4] };
          }
          if (output.type === 'scatter3D' || output.type === 'scatter') {
            output.symbolSize = function (value) { return value[4]; };
          }
        } else {
          has2DSeries = true;
          output.type = currentEffectiveTemplate.indexOf('Scatter') >= 0 ? 'scatter' : 'line';
          output.data = clone(input.data);
          output.encode = { x: 0, y: 1 };
        }
        output.silent = currentInteractionMode === 'Disabled';
        return output;
      });

      const names = payload.series.map(function (series) { return series.name; });
      const legends = Array.isArray(option.legend) ? option.legend : [option.legend || {}];
      legends.forEach(function (legend) { legend.data = names; });
      option.legend = Array.isArray(option.legend) ? legends : legends[0];

      if (has2DSeries) {
        if (useHeatmapAxes) {
          updateAxis(option, 'xAxis', { type: 'category', data: heatmapXDomain });
          updateAxis(option, 'yAxis', { type: 'category', data: heatmapYDomain });
        } else if (payload.xAxisMode === 'Category' || categoryLabels.length > 0) {
          updateAxis(option, 'xAxis', { type: 'category', data: categoryLabels });
          updateAxis(option, 'yAxis', { type: 'value' });
        } else {
          updateAxis(option, 'xAxis', { type: 'value', data: undefined });
          updateAxis(option, 'yAxis', { type: 'value' });
        }
        if (!option.grid) option.grid = { left: 58, right: 24, top: 42, bottom: 44 };
      }
      if (currentEffectiveTemplate === 'Bar3DHeightMap' &&
          payload.series.some(function (series) { return series.type === 'data3D'; })) {
        updateAxis(option, 'xAxis3D', { type: 'value', data: undefined });
        updateAxis(option, 'yAxis3D', { type: 'value', data: undefined });
        updateAxis(option, 'zAxis3D', { type: 'value', data: undefined });
      }
      return window.UEEChartsTemplates.applyInteractionMode(option, currentInteractionMode);
    }
    function resizeChart() {
      if (!chart) return;
      chart.resize();
      if (window.__UE_ECHARTS_TEST_RESIZE_PROBE__ === true) {
        emit('TEST_RESIZED', String(Math.round(chart.getWidth())) + ':' + String(Math.round(chart.getHeight())));
      }
    }
    const hostApi = {
      renderTemplate: function (template, payload, interactionMode) {
        try {
          const result = window.UEEChartsTemplates.createTemplate(template, payload || {}, webglAvailable);
          const option = window.UEEChartsTemplates.applyInteractionMode(result.option, interactionMode || 'ClickOnly');
          if (result.effectiveTemplate !== result.requestedTemplate) {
            emit('WARNING', 'WebGL unavailable; using ' + result.effectiveTemplate + ' for ' + result.requestedTemplate);
          }
          chart.clear();
          chart.setOption(option, { notMerge: true, lazyUpdate: false });
          currentEffectiveTemplate = result.effectiveTemplate;
          templateBaseOption = clone(option);
          currentOption = option;
          currentPayload = null;
          currentInteractionMode = interactionMode || 'ClickOnly';
          if (parameters.get('testSeriesProbe') === '1') {
            const appliedOption = chart.getOption();
            const seriesCount = Array.isArray(appliedOption.series) ? appliedOption.series.length : 0;
            emit('TEST_SERIES_COUNT', String(seriesCount));
          }
          emit('RENDERED', result.requestedTemplate + ':' + result.effectiveTemplate);
          return result.effectiveTemplate;
        } catch (error) {
          const detail = error && error.stack ? error.stack.replace(/[\r\n]+/g, ' | ') : (error && error.message ? error.message : String(error));
          emit('ERROR', template + ' render failed: ' + detail);
          return null;
        }
      },
      setInteractionMode: function (requestId, interactionMode) {
        try {
          if (!validRequestId(requestId)) throw new Error('Invalid interaction request id');
          if (!['Disabled', 'ClickOnly', 'FullHover'].includes(interactionMode)) {
            throw new Error('Invalid interaction mode');
          }
          if (!currentOption) throw new Error('Chart option is not ready');
          const option = window.UEEChartsTemplates.applyInteractionMode(currentOption, interactionMode);
          chart.setOption(option, { notMerge: true, lazyUpdate: false });
          currentOption = option;
          currentInteractionMode = interactionMode;
          emitResult('INTERACTION_RESULT', requestId, true, interactionMode);
          return true;
        } catch (error) {
          const detail = error && error.message ? error.message : String(error);
          emitResult('INTERACTION_RESULT', requestId, false, detail);
          return false;
        }
      },
      applyOptionBase64: function (requestId, base64) {
        try {
          if (!validRequestId(requestId)) throw new Error('Invalid option request id');
          const optionValue = JSON.parse(decodeBase64Text(base64, 16 * 1024 * 1024, 'option payload'));
          if (!optionValue || typeof optionValue !== 'object' || Array.isArray(optionValue)) {
            throw new Error('ECharts option JSON must be an object');
          }
          const option = window.UEEChartsTemplates.applyInteractionMode(optionValue, currentInteractionMode);
          chart.setOption(option, { notMerge: true, lazyUpdate: false });
          currentEffectiveTemplate = 'CustomOption';
          templateBaseOption = clone(option);
          currentOption = option;
          currentPayload = null;
          emitResult('OPTION_RESULT', requestId, true, 'CustomOption applied');
          return true;
        } catch (error) {
          const detail = error && error.message ? error.message : String(error);
          emitResult('OPTION_RESULT', requestId, false, detail);
          return false;
        }
      },
      executeJavaScriptBase64: function (requestId, base64) {
        try {
          if (!validRequestId(requestId)) throw new Error('Invalid JavaScript request id');
          const source = decodeBase64Text(base64, 1024 * 1024, 'JavaScript payload');
          if (source.trim().length === 0) throw new Error('JavaScript source is empty');
          const execute = new Function('chart', 'echarts', 'host', '"use strict";\n' + source);
          execute.call(undefined, chart, window.echarts, hostApi);
          emitResult('JAVASCRIPT_RESULT', requestId, true, 'JavaScript executed');
          return true;
        } catch (error) {
          const detail = error && error.message ? error.message : String(error);
          emitResult('JAVASCRIPT_RESULT', requestId, false, detail);
          return false;
        }
      },
      applyDataBase64: function (base64) {
        try {
          const payload = decodePayload(base64);
          const pointCount = validatePayload(payload);
          const option = optionForPayload(payload);
          chart.setOption(option, { notMerge: true, lazyUpdate: false });
          currentOption = option;
          currentPayload = clone(payload);
          emit('APPLIED', String(payload.revision) + ':' + String(pointCount));
          return true;
        } catch (error) {
          const detail = error && error.message ? error.message : String(error);
          emit('ERROR', 'data apply failed: ' + detail.replace(/[\r\n]+/g, ' | '));
          return false;
        }
      },
      applyStreamDeltaBase64: function (base64) {
        try {
          const delta = decodePayload(base64);
          if (!currentPayload || !Number.isSafeInteger(delta.revision) || delta.revision <= 0 ||
              !Number.isSafeInteger(delta.baseRevision) || delta.baseRevision !== currentPayload.revision ||
              !Number.isSafeInteger(delta.drop) || delta.drop < 0 || !delta.series ||
              delta.series.index !== 0 || delta.series.type !== currentPayload.series[0].type ||
              !Array.isArray(delta.series.data) || delta.series.data.length > 256 ||
              delta.drop > currentPayload.series[0].data.length) {
            throw new Error('Invalid or stale stream delta');
          }
          const candidate = clone(currentPayload);
          candidate.revision = delta.revision;
          candidate.series[0].data.splice(0, delta.drop);
          candidate.series[0].data.push.apply(candidate.series[0].data, clone(delta.series.data));
          const pointCount = validatePayload(candidate);
          const option = optionForPayload(candidate);
          chart.setOption(option, { notMerge: true, lazyUpdate: false });
          currentOption = option;
          currentPayload = candidate;
          emit('APPLIED', String(candidate.revision) + ':' + String(pointCount));
          return true;
        } catch (error) {
          const detail = error && error.message ? error.message : String(error);
          emit('ERROR', 'stream delta apply failed: ' + detail.replace(/[\r\n]+/g, ' | '));
          return false;
        }
      },
      getOptionForTesting: function () {
        return chart.getOption();
      },
      getGraphicShapeStatsForTesting: function () {
        const displayList = chart.getZr().storage.getDisplayList(true);
        const rects = displayList.filter(function (item) { return item.type === 'rect' && item.shape; });
        return {
          heatmapRectCount: rects.length,
          allFinite: rects.every(function (item) {
            return ['x', 'y', 'width', 'height'].every(function (field) {
              return Number.isFinite(item.shape[field]);
            });
          })
        };
      },
      getGraphicBoundsStatsForTesting: function () {
        const bounds = window.UEEChartsHost && chart
          ? chart.getZr().storage.getDisplayList(true)
            .filter(function (item) { return typeof item.getBoundingRect === 'function'; })
            .map(function (item) { return item.getBoundingRect(); })
          : [];
        return {
          count: bounds.length,
          allFinite: bounds.every(function (box) {
            return ['x', 'y', 'width', 'height'].every(function (field) { return Number.isFinite(box[field]); });
          }),
          hasNonZero: bounds.some(function (box) { return box.width > 0 && box.height > 0; })
        };
      },
      getSeriesCoordinateStatsForTesting: function (value) {
        try {
          const seriesModel = chart.getModel().getSeriesByIndex(0);
          const coordinateSystem = seriesModel && seriesModel.coordinateSystem;
          if (!coordinateSystem || typeof coordinateSystem.dataToPoint !== 'function') {
            return { allFinite: false, count: 0 };
          }
          const point = Array.from(coordinateSystem.dataToPoint(value));
          return { allFinite: point.length > 0 && point.every(Number.isFinite), count: point.length };
        } catch (_) {
          return { allFinite: false, count: 0 };
        }
      },
      resize: resizeChart,
      dispose: function () {
        if (resizeObserver) {
          resizeObserver.disconnect();
          resizeObserver = null;
        }
        if (chart) {
          chart.dispose();
          chart = null;
        }
        if (window.UEEChartsHost === hostApi) {
          delete window.UEEChartsHost;
        }
      }
    };
    resizeObserver = new ResizeObserver(resizeChart);
    resizeObserver.observe(chartElement);
    window.UEEChartsHost = hostApi;
    emit('READY');
  } catch (error) {
    emit('ERROR', error && error.message ? error.message : String(error));
  }
}());
