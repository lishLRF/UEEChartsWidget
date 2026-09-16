(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  if (root) root.UEEChartsTemplates = api;
}(typeof window !== 'undefined' ? window : globalThis, function () {
  'use strict';

  const MAX_HEIGHT_MAP_SIDE = 128;
  const MAX_GENERATED_POINTS = MAX_HEIGHT_MAP_SIDE * MAX_HEIGHT_MAP_SIDE;

  function clone(value) {
    if (Array.isArray(value)) return value.map(clone);
    if (value && typeof value === 'object') {
      const result = {};
      Object.keys(value).forEach(function (key) { result[key] = clone(value[key]); });
      return result;
    }
    return value;
  }

  function detectWebGL(documentRef) {
    try {
      const owner = documentRef || (typeof document !== 'undefined' ? document : null);
      if (!owner || typeof owner.createElement !== 'function') return false;
      const canvas = owner.createElement('canvas');
      return !!(canvas.getContext('webgl2') || canvas.getContext('webgl') || canvas.getContext('experimental-webgl'));
    } catch (_) {
      return false;
    }
  }

  function selectEffectiveTemplate(requestedTemplate, webglAvailable) {
    if (!webglAvailable && requestedTemplate === 'Bar3DHeightMap') return 'Bar3DHeightMap2D';
    if (!webglAvailable && requestedTemplate === 'DataTableScatter3D') return 'DataTableScatter2D';
    return requestedTemplate;
  }

  function segmentedAreaLine(payload) {
    const xAxis = Array.isArray(payload.xAxis) ? payload.xAxis : ['00:00', '04:00', '08:00', '12:00', '16:00', '20:00', '24:00'];
    const values = Array.isArray(payload.values) ? payload.values : [120, 176, 132, 248, 196, 286, 232];
    const oneThird = Math.max(1, Math.floor((xAxis.length - 1) / 3));
    const twoThirds = Math.max(2, Math.floor((xAxis.length - 1) * 2 / 3));
    return {
      animation: false, backgroundColor: 'transparent', tooltip: { trigger: 'axis' },
      legend: { data: [payload.seriesName || 'Value'] },
      grid: { left: 48, right: 24, top: 42, bottom: 36 },
      xAxis: { type: 'category', boundaryGap: false, data: xAxis }, yAxis: { type: 'value' },
      series: [{
        name: payload.seriesName || 'Value', type: 'line', smooth: true, symbolSize: 7,
        data: values, lineStyle: { width: 3 }, areaStyle: { opacity: 0.25 },
        markArea: { silent: true, data: [
          [{ name: 'Low', xAxis: xAxis[0], itemStyle: { color: 'rgba(84,112,198,0.10)' } }, { xAxis: xAxis[oneThird] }],
          [{ name: 'Mid', xAxis: xAxis[oneThird] }, { xAxis: xAxis[twoThirds] }],
          [{ name: 'High', xAxis: xAxis[twoThirds] }, { xAxis: xAxis[xAxis.length - 1] }]
        ] }
      }]
    };
  }

  function deterministicHeightData(size) {
    const points = [];
    for (let x = 0; x < size; x += 1) {
      for (let y = 0; y < size; y += 1) {
        const wave = Math.sin(x * 0.72) * Math.cos(y * 0.58);
        const ridge = Math.sin((x + y) * 0.31) * 0.5;
        points.push([x, y, Number(((wave + ridge + 1.6) * 18).toFixed(3))]);
      }
    }
    return points;
  }

  function heightMapInput(payload) {
    const size = payload.size === undefined ? 10 : payload.size;
    if (!Number.isFinite(size) || !Number.isInteger(size) || size < 1 || size > MAX_HEIGHT_MAP_SIDE) {
      throw new Error('Bar3DHeightMap size must be an integer between 1 and ' + MAX_HEIGHT_MAP_SIDE + '.');
    }
    const hasData = Array.isArray(payload.data);
    return {
      size: size,
      data: hasData ? payload.data : deterministicHeightData(size),
      categories: Array.from({ length: size }, function (_, index) { return String(index); })
    };
  }

  function bar3DHeightMap(payload) {
    const input = heightMapInput(payload);
    return {
      animation: false, backgroundColor: 'transparent', tooltip: {}, legend: { data: ['Height'] },
      visualMap: { max: 60, inRange: { color: ['#313695', '#74add1', '#ffffbf', '#f46d43', '#a50026'] } },
      xAxis3D: { type: 'category', data: input.categories }, yAxis3D: { type: 'category', data: input.categories },
      zAxis3D: { type: 'value' }, grid3D: { boxWidth: 120, boxDepth: 120, viewControl: { autoRotate: false } },
      series: [{ name: 'Height', type: 'bar3D', data: input.data, shading: 'lambert' }]
    };
  }

  function bar3DHeightMap2D(payload) {
    const input = heightMapInput(payload);
    return {
      animation: false, backgroundColor: 'transparent', tooltip: { position: 'top' }, legend: { data: ['Height'] },
      grid: { left: 48, right: 24, top: 42, bottom: 42 },
      xAxis: { type: 'category', data: input.categories, splitArea: { show: true } },
      yAxis: { type: 'category', data: input.categories, splitArea: { show: true } },
      visualMap: { min: 0, max: 60, calculable: true, orient: 'horizontal', left: 'center', bottom: 0 },
      series: [{ name: 'Height', type: 'heatmap', data: input.data }]
    };
  }

  function nutrients(payload) {
    return Array.isArray(payload.data) ? payload.data : [
      [6.8, 64.1, 0.5, 'Apple'], [1.1, 22.8, 0.3, 'Banana'], [25.4, 3.5, 20.0, 'Almond'],
      [3.2, 4.7, 3.3, 'Milk'], [31.0, 0.0, 3.6, 'Tuna'], [12.6, 1.1, 10.6, 'Egg'],
      [2.8, 6.6, 0.4, 'Broccoli'], [4.4, 23.0, 1.9, 'Oats'], [0.7, 10.5, 0.2, 'Orange']
    ];
  }

  function dataTableScatter3D(payload) {
    return {
      animation: false, backgroundColor: 'transparent', tooltip: {}, legend: { data: ['Foods'] },
      xAxis3D: { name: 'Protein', type: 'value' }, yAxis3D: { name: 'Carbohydrate', type: 'value' },
      zAxis3D: { name: 'Fat', type: 'value' }, grid3D: { viewControl: { autoRotate: false } },
      series: [{ name: 'Foods', type: 'scatter3D', symbolSize: 12, data: nutrients(payload), encode: { x: 0, y: 1, z: 2, tooltip: [0, 1, 2, 3] } }]
    };
  }

  function dataTableScatter2D(payload) {
    const data = nutrients(payload);
    return {
      animation: false, backgroundColor: 'transparent', tooltip: {}, legend: { data: ['Foods'] },
      grid: { left: 58, right: 24, top: 42, bottom: 44 }, xAxis: { name: 'Protein', type: 'value' },
      yAxis: { name: 'Carbohydrate', type: 'value' },
      series: [{ name: 'Foods', type: 'scatter', symbolSize: 12,
        data: data.map(function (row) { return [row[0], row[1], row[3], row[2]]; }),
        encode: { x: 0, y: 1, tooltip: [0, 1, 3, 2] } }]
    };
  }

  function customOption(payload) {
    const option = clone(payload && payload.option ? payload.option : {});
    option.animation = false;
    option.backgroundColor = option.backgroundColor || 'transparent';
    return option;
  }

  function createTemplate(requestedTemplate, payload, webglAvailable) {
    const safePayload = payload && typeof payload === 'object' ? payload : {};
    const effectiveTemplate = selectEffectiveTemplate(requestedTemplate, webglAvailable);
    const factories = { SegmentedAreaLine: segmentedAreaLine, Bar3DHeightMap: bar3DHeightMap,
      Bar3DHeightMap2D: bar3DHeightMap2D, DataTableScatter3D: dataTableScatter3D,
      DataTableScatter2D: dataTableScatter2D, CustomOption: customOption };
    if (!factories[effectiveTemplate]) throw new Error('Unknown template: ' + requestedTemplate);
    const option = factories[effectiveTemplate](safePayload);
    option.animation = false;
    return { requestedTemplate: requestedTemplate, effectiveTemplate: effectiveTemplate, option: option };
  }

  function applyInteractionMode(sourceOption, interactionMode) {
    const option = clone(sourceOption);
    const visited = new WeakSet();

    function components(value) {
      if (Array.isArray(value)) return value.filter(function (item) { return item && typeof item === 'object'; });
      return value && typeof value === 'object' ? [value] : [];
    }

    function applyScope(scope) {
      if (!scope || typeof scope !== 'object' || Array.isArray(scope) || visited.has(scope)) return;
      visited.add(scope);
      const disabled = interactionMode === 'Disabled';
      const fullHover = interactionMode === 'FullHover';

      components(scope.tooltip).forEach(function (tooltip) {
        tooltip.show = !disabled;
        tooltip.triggerOn = disabled ? 'none' : (fullHover ? 'mousemove|click' : 'click');
      });
      components(scope.legend).forEach(function (legend) { legend.selectedMode = !disabled; });

      if (!Array.isArray(scope.series)) {
        scope.series = scope.series && typeof scope.series === 'object' ? [scope.series] : [];
      }
      scope.series.forEach(function (series) {
        if (series && typeof series === 'object') series.silent = disabled;
      });

      components(scope.grid3D).forEach(function (grid3D) {
        if (!grid3D.viewControl || typeof grid3D.viewControl !== 'object' || Array.isArray(grid3D.viewControl)) {
          grid3D.viewControl = {};
        }
        grid3D.viewControl.rotateSensitivity = disabled ? 0 : 1;
        grid3D.viewControl.zoomSensitivity = disabled || !fullHover ? 0 : 1;
        grid3D.viewControl.panSensitivity = disabled || !fullHover ? 0 : 1;
        grid3D.viewControl.autoRotate = false;
      });

      applyScope(scope.baseOption);
      if (Array.isArray(scope.options)) scope.options.forEach(applyScope);
      if (Array.isArray(scope.media)) {
        scope.media.forEach(function (mediaItem) {
          if (mediaItem && typeof mediaItem === 'object') applyScope(mediaItem.option);
        });
      }
    }

    applyScope(option);
    return option;
  }

  return { applyInteractionMode: applyInteractionMode, createTemplate: createTemplate,
    detectWebGL: detectWebGL, selectEffectiveTemplate: selectEffectiveTemplate,
    limits: { maxHeightMapSide: MAX_HEIGHT_MAP_SIDE, maxGeneratedPoints: MAX_GENERATED_POINTS } };
}));
