/* =============================================================================
   app.js —— 文章查重系统前端逻辑
   - 文件选择 / 拖拽上传
   - 示例语料勾选与查重
   - 结果渲染：统计卡片、文档查重率表、相似文档对与重复片段对照
   ========================================================================== */

(function () {
  'use strict';

  // ---------------------------------------------------------------------------
  // 全局状态
  // ---------------------------------------------------------------------------
  const state = {
    files: [],            // 用户选择的文件（File 对象）
    samples: [],          // 示例语料
    selectedSamples: new Set(),
    result: null,         // 最近一次查重结果
    expandedPairs: new Set(),
    focusDoc: null,       // 当前聚焦的文档下标
  };

  const $ = (id) => document.getElementById(id);

  // ---------------------------------------------------------------------------
  // 小工具
  // ---------------------------------------------------------------------------
  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function formatPercent(rate, digits) {
    const value = (Number(rate) || 0) * 100;
    return value.toFixed(digits === undefined ? 2 : digits) + '%';
  }

  function formatNumber(value) {
    return new Intl.NumberFormat('zh-CN').format(value || 0);
  }

  function levelClass(level) {
    return 'lv' + Math.max(0, Math.min(3, Number(level) || 0));
  }

  function docTypeClass(type) {
    if (type === 'docx') return 'docx';
    if (type === 'pdf') return 'pdf';
    return 'txt';
  }

  function docTypeLabel(type) {
    if (type === 'docx') return 'DOCX';
    if (type === 'pdf') return 'PDF';
    return 'TXT';
  }

  // ---------------------------------------------------------------------------
  // 服务状态
  // ---------------------------------------------------------------------------
  async function checkHealth() {
    const dot = $('server-dot');
    const label = $('server-status');
    try {
      const response = await fetch('/api/health');
      const data = await response.json();
      if (data.ok) {
        dot.classList.add('ok');
        dot.classList.remove('bad');
        label.textContent = '服务运行中';
      } else {
        throw new Error('bad health');
      }
    } catch (error) {
      dot.classList.add('bad');
      dot.classList.remove('ok');
      label.textContent = '服务未响应';
    }
  }

  // ---------------------------------------------------------------------------
  // 文件选择
  // ---------------------------------------------------------------------------
  const ACCEPT_EXT = ['docx', 'pdf', 'txt', 'md'];

  function extensionOf(name) {
    const dot = name.lastIndexOf('.');
    return dot === -1 ? '' : name.slice(dot + 1).toLowerCase();
  }

  function addFiles(fileList) {
    const rejected = [];
    Array.from(fileList).forEach((file) => {
      const ext = extensionOf(file.name);
      if (ACCEPT_EXT.indexOf(ext) === -1) {
        rejected.push(file.name + '（不支持的类型）');
        return;
      }
      const duplicated = state.files.some((item) => item.name === file.name && item.size === file.size);
      if (duplicated) return;
      state.files.push(file);
    });
    renderFiles();
    if (rejected.length) {
      alert('以下文件已跳过：\n' + rejected.join('\n'));
    }
  }

  function renderFiles() {
    const list = $('file-list');
    list.innerHTML = '';
    state.files.forEach((file, index) => {
      const chip = el('div', 'file-chip');
      const type = el('div', 'ftype ' + docTypeClass(extensionOf(file.name)), docTypeLabel(extensionOf(file.name)));
      const info = el('div');
      info.appendChild(el('div', 'fname', file.name));
      info.appendChild(el('div', 'fsize', humanSize(file.size)));
      const remove = el('button', 'remove', '×');
      remove.type = 'button';
      remove.title = '移除';
      remove.addEventListener('click', () => {
        state.files.splice(index, 1);
        renderFiles();
      });
      chip.appendChild(type);
      chip.appendChild(info);
      chip.appendChild(remove);
      list.appendChild(chip);
    });

    const enough = state.files.length >= 2;
    $('analyze-btn').disabled = !enough;
    $('analyze-btn').textContent = enough
      ? '开始查重（' + state.files.length + ' 篇）'
      : '开始查重';
  }

  function humanSize(bytes) {
    if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB';
    if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return bytes + ' B';
  }

  // ---------------------------------------------------------------------------
  // 示例语料
  // ---------------------------------------------------------------------------
  async function loadSamples() {
    const list = $('sample-list');
    try {
      const response = await fetch('/api/samples');
      const data = await response.json();
      state.samples = data.files || [];
    } catch (error) {
      state.samples = [];
    }
    renderSamples();
  }

  function renderSamples() {
    const list = $('sample-list');
    list.innerHTML = '';
    if (!state.samples.length) {
      list.appendChild(el('p', 'muted', '未找到示例语料。把测试文档放进 samples/ 目录后点击“刷新”。'));
      $('sample-analyze-btn').disabled = true;
      return;
    }

    state.samples.forEach((sample) => {
      const item = el('label', 'sample-item' + (state.selectedSamples.has(sample.name) ? ' checked' : ''));
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = state.selectedSamples.has(sample.name);
      box.addEventListener('change', () => {
        if (box.checked) state.selectedSamples.add(sample.name);
        else state.selectedSamples.delete(sample.name);
        item.classList.toggle('checked', box.checked);
        updateSampleButton();
      });
      item.appendChild(box);
      item.appendChild(el('span', 'name', sample.name));
      item.appendChild(el('span', 'meta', docTypeLabel(sample.type) + ' · ' + sample.sizeText));
      list.appendChild(item);
    });
    updateSampleButton();
  }

  function updateSampleButton() {
    const count = state.selectedSamples.size;
    const button = $('sample-analyze-btn');
    button.disabled = count < 2;
    button.textContent = count >= 2 ? '使用示例查重（' + count + ' 篇）' : '使用示例查重';
    $('sample-hint').textContent =
      count >= 2
        ? '已选择 ' + count + ' 篇示例文档。'
        : '勾选任意两篇及以上文档，使用示例查重。';
  }

  // ---------------------------------------------------------------------------
  // 请求与遮罩
  // ---------------------------------------------------------------------------
  function showOverlay(subText) {
    $('overlay-sub').textContent = subText || '解析文件、提取正文、计算指纹';
    $('overlay').classList.remove('hidden');
  }

  function hideOverlay() {
    $('overlay').classList.add('hidden');
  }

  function queryString() {
    const minMatch = $('sensitivity').value;
    return '?minMatch=' + encodeURIComponent(minMatch);
  }

  async function postAnalyze(url, options) {
    showOverlay();
    try {
      const response = await fetch(url, options);
      const text = await response.text();
      let data;
      try {
        data = JSON.parse(text);
      } catch (error) {
        throw new Error('服务返回了非 JSON 响应（HTTP ' + response.status + '）');
      }
      if (!data.ok) {
        const failures = (data.failures || []).join('\n');
        throw new Error((data.error || '查重失败') + (failures ? '\n' + failures : ''));
      }
      state.result = data;
      state.expandedPairs.clear();
      state.focusDoc = null;
      renderResult();
    } catch (error) {
      alert('查重未完成：\n' + error.message);
    } finally {
      hideOverlay();
    }
  }

  async function analyzeUploads() {
    if (state.files.length < 2) return;
    const form = new FormData();
    state.files.forEach((file) => form.append('files', file, file.name));
    await postAnalyze('/api/analyze' + queryString(), { method: 'POST', body: form });
  }

  async function analyzeSamples() {
    if (state.selectedSamples.size < 2) return;
    const body = JSON.stringify(Array.from(state.selectedSamples));
    await postAnalyze('/api/analyze-samples' + queryString(), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body,
    });
  }

  // ---------------------------------------------------------------------------
  // 结果渲染
  // ---------------------------------------------------------------------------
  function renderResult() {
    const result = state.result;
    if (!result) return;

    $('result-panel').classList.remove('hidden');
    renderSummary(result);
    renderStats(result);
    renderFailures(result);
    renderDocTable(result);

    // 默认展开相似度最高的一组，方便立刻看到重复位置
    const topPair = (result.pairs || [])[0];
    if (topPair) state.expandedPairs.add(topPair.id);
    renderPairs(result);

    $('result-panel').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  function renderSummary(result) {
    const docs = result.documents || [];
    const pairs = result.pairs || [];
    $('result-summary').textContent =
      '共比对 ' + docs.length + ' 篇文档，发现 ' + pairs.length + ' 组相似文档对；耗时 ' +
      Number(result.elapsedMs || 0).toFixed(0) + ' 毫秒。';
    $('elapsed-badge').textContent = '耗时 ' + Number(result.elapsedMs || 0).toFixed(1) + ' ms';
  }

  function renderStats(result) {
    const docs = result.documents || [];
    const pairs = result.pairs || [];
    const rates = docs.map((doc) => Number(doc.dupRate) || 0);
    const avg = rates.length ? rates.reduce((sum, value) => sum + value, 0) / rates.length : 0;
    const max = rates.length ? Math.max.apply(null, rates) : 0;
    const maxPair = pairs.length ? Number(pairs[0].similarity) || 0 : 0;

    const container = $('stats');
    container.innerHTML = '';
    const items = [
      { label: '文档数量', value: docs.length, unit: '篇' },
      { label: '内容字符总数', value: formatNumber(result.totalChars), unit: '' },
      { label: '平均查重率', value: formatPercent(avg, 1), unit: '' },
      { label: '最高查重率', value: formatPercent(max, 1), unit: '' },
      { label: '最高相似度文档对', value: formatPercent(maxPair, 1), unit: '' },
    ];
    items.forEach((item) => {
      const card = el('div', 'stat');
      card.appendChild(el('div', 'label', item.label));
      const value = el('div', 'value', item.value);
      if (item.unit) value.appendChild(el('small', '', item.unit));
      card.appendChild(value);
      container.appendChild(card);
    });
  }

  function renderFailures(result) {
    const box = $('failure-box');
    const failures = result.failures || [];
    if (!failures.length) {
      box.classList.add('hidden');
      box.innerHTML = '';
      return;
    }
    box.classList.remove('hidden');
    box.innerHTML = '';
    box.appendChild(el('strong', '', '以下文件未能参与比对：'));
    const list = document.createElement('ul');
    failures.forEach((item) => list.appendChild(el('li', '', item)));
    box.appendChild(list);
  }

  function renderDocTable(result) {
    const body = $('doc-tbody');
    body.innerHTML = '';
    (result.documents || []).forEach((doc) => {
      const row = document.createElement('tr');

      row.appendChild(el('td', 'num', doc.index + 1));

      const nameCell = document.createElement('td');
      const nameWrap = el('div', 'doc-name');
      nameWrap.title = doc.name;
      nameWrap.appendChild(el('span', 'ftype ' + docTypeClass(doc.type), docTypeLabel(doc.type)));
      nameWrap.appendChild(el('span', 'name', doc.name));
      nameCell.appendChild(nameWrap);
      row.appendChild(nameCell);

      row.appendChild(el('td', '', docTypeLabel(doc.type)));
      row.appendChild(el('td', 'num', formatNumber(doc.chars)));
      row.appendChild(el('td', 'num', formatNumber(doc.dupChars)));

      // 查重率进度条
      const rateCell = el('td', 'rate-cell');
      const bar = el('div', 'rate-bar');
      const fill = document.createElement('i');
      fill.className = levelClass(doc.level);
      fill.style.width = Math.min(100, Math.max(1.5, Number(doc.dupRate) * 100)).toFixed(1) + '%';
      bar.appendChild(fill);
      const text = el('div', 'rate-text');
      const strong = el('strong', '', formatPercent(doc.dupRate, 1));
      text.appendChild(strong);
      text.appendChild(document.createTextNode('（' + formatNumber(doc.dupChars) + ' 字）'));
      rateCell.appendChild(bar);
      rateCell.appendChild(text);
      row.appendChild(rateCell);

      const levelCell = document.createElement('td');
      levelCell.appendChild(el('span', 'badge ' + levelClass(doc.level), doc.levelName));
      row.appendChild(levelCell);

      // 最高相似文档
      const otherCell = document.createElement('td');
      const pair = (result.pairs || [])[doc.maxPairIndex];
      if (pair) {
        const otherIndex = pair.a === doc.index ? pair.b : pair.a;
        const other = (result.documents || [])[otherIndex];
        const rate = pair.a === doc.index ? pair.rateA : pair.rateB;
        if (other) {
          otherCell.appendChild(document.createTextNode(
            other.name.length > 22 ? other.name.slice(0, 22) + '…' : other.name
          ));
          otherCell.appendChild(el('div', 'muted', '重复 ' + formatPercent(rate, 1)));
        }
      } else {
        otherCell.appendChild(el('span', 'muted', '无明显相似'));
      }
      row.appendChild(otherCell);

      const actionCell = document.createElement('td');
      if ((doc.pairIds || []).length) {
        const button = el('button', 'link-btn', '定位片段');
        button.type = 'button';
        button.addEventListener('click', () => focusDocument(doc.index));
        actionCell.appendChild(button);
      }
      row.appendChild(actionCell);

      if (state.focusDoc === doc.index) row.style.background = '#f5f6ff';
      body.appendChild(row);
    });
  }

  function focusDocument(docIndex) {
    state.focusDoc = docIndex;
    renderDocTable(state.result);
    renderPairs(state.result);
    const panel = $('pair-list');
    if (panel) panel.scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  function renderPairs(result) {
    const container = $('pair-list');
    container.innerHTML = '';
    let pairs = result.pairs || [];
    if (state.focusDoc !== null) {
      pairs = pairs.filter((pair) => pair.a === state.focusDoc || pair.b === state.focusDoc);
    }
    $('pair-count').textContent = pairs.length ? '（' + pairs.length + ' 组）' : '';

    if (!pairs.length) {
      container.appendChild(el('p', 'muted', '没有发现达到阈值的相似内容。'));
      return;
    }

    pairs.forEach((pair) => {
      const docA = result.documents[pair.a];
      const docB = result.documents[pair.b];
      const card = el('div', 'pair-card');

      const head = el('div', 'pair-head');
      const title = el('div', 'pair-title');
      const spanA = el('span', '', shortName(docA.name));
      const spanB = el('span', '', shortName(docB.name));
      spanA.title = docA.name;
      spanB.title = docB.name;
      title.appendChild(spanA);
      title.appendChild(el('span', 'arrow', '↔'));
      title.appendChild(spanB);
      head.appendChild(title);

      const metrics = el('div', 'pair-metrics');
      metrics.appendChild(el('span', 'badge neutral', '相似度 ' + formatPercent(pair.similarity, 1)));
      metrics.appendChild(el('span', 'badge jaccard', 'Jaccard ' + Number(pair.jaccard).toFixed(3)));
      metrics.appendChild(el('span', 'badge jaccard', '包含度 ' + Number(pair.containment).toFixed(3)));
      metrics.appendChild(el('span', 'badge jaccard', 'MinHash ' + Number(pair.minhash).toFixed(3)));
      metrics.appendChild(el('span', 'badge neutral', 'SimHash 距 ' + pair.simhashDistance));
      head.appendChild(metrics);
      card.appendChild(head);

      const expanded = state.expandedPairs.has(pair.id);
      const body = el('div', 'pair-body');
      if (expanded) {
        body.appendChild(renderPairBody(result, pair, docA, docB));
      } else {
        body.classList.add('hidden');
      }
      card.appendChild(body);

      head.addEventListener('click', () => {
        if (state.expandedPairs.has(pair.id)) state.expandedPairs.delete(pair.id);
        else state.expandedPairs.add(pair.id);
        renderPairs(result);
      });

      container.appendChild(card);
    });
  }

  function shortName(name) {
    return name.length > 30 ? name.slice(0, 30) + '…' : name;
  }

  // 把最近一次查重结果导出为 JSON 文件，便于存档或二次分析
  function exportReport() {
    if (!state.result) return;
    const payload = Object.assign({}, state.result, {
      exportedAt: new Date().toISOString(),
      sensitivity: $('sensitivity').value,
    });
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
    link.href = url;
    link.download = '查重报告-' + stamp + '.json';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
  }

  function renderPairBody(result, pair, docA, docB) {
    const wrapper = document.createDocumentFragment();

    const toolbar = el('div', 'seg-toolbar');
    toolbar.appendChild(el('span', '',
      '共 ' + pair.segments.length + ' 处重复片段（按长度降序）；A 侧重复 ' +
      formatPercent(pair.rateA, 1) + '，B 侧重复 ' + formatPercent(pair.rateB, 1)));
    toolbar.appendChild(el('span', '', '重复字符 ' + formatNumber(pair.dupChars)));
    wrapper.appendChild(toolbar);

    pair.segments.forEach((seg) => {
      const box = el('div', 'seg');
      const grid = el('div', 'seg-grid');
      grid.appendChild(renderSegmentSide(docA, seg, 'a'));
      grid.appendChild(renderSegmentSide(docB, seg, 'b'));
      box.appendChild(grid);
      wrapper.appendChild(box);
    });
    return wrapper;
  }

  function renderSegmentSide(doc, seg, side) {
    const wrap = el('div', 'seg-side');
    const label = el('div', 'seg-label');
    label.appendChild(el('span', '', doc.name.length > 32 ? doc.name.slice(0, 32) + '…' : doc.name));
    label.appendChild(el('span', '', '第 ' + seg[side + 'Line'] + ' 行 · ' + seg.chars + ' 字'));
    wrap.appendChild(label);

    const text = el('div', 'seg-text');
    if (seg[side + 'Before']) text.appendChild(document.createTextNode(seg[side + 'Before']));
    const mark = el('mark', '', seg[side + 'Match']);
    text.appendChild(mark);
    if (seg[side + 'After']) text.appendChild(document.createTextNode(seg[side + 'After']));
    wrap.appendChild(text);
    return wrap;
  }

  // ---------------------------------------------------------------------------
  // 事件绑定
  // ---------------------------------------------------------------------------
  function bindEvents() {
    const dropzone = $('dropzone');
    const input = $('file-input');

    dropzone.addEventListener('click', () => input.click());
    input.addEventListener('change', () => {
      if (input.files && input.files.length) addFiles(input.files);
      input.value = '';
    });

    ['dragenter', 'dragover'].forEach((type) => {
      dropzone.addEventListener(type, (event) => {
        event.preventDefault();
        dropzone.classList.add('dragover');
      });
    });
    ['dragleave', 'drop'].forEach((type) => {
      dropzone.addEventListener(type, (event) => {
        event.preventDefault();
        dropzone.classList.remove('dragover');
      });
    });
    dropzone.addEventListener('drop', (event) => {
      if (event.dataTransfer && event.dataTransfer.files.length) addFiles(event.dataTransfer.files);
    });

    $('clear-btn').addEventListener('click', () => {
      state.files = [];
      renderFiles();
    });

    $('analyze-btn').addEventListener('click', analyzeUploads);
    $('sample-analyze-btn').addEventListener('click', analyzeSamples);
    $('refresh-samples').addEventListener('click', loadSamples);
    $('export-btn').addEventListener('click', exportReport);
  }

  // ---------------------------------------------------------------------------
  // 启动
  // ---------------------------------------------------------------------------
  function init() {
    bindEvents();
    renderFiles();
    checkHealth();
    loadSamples();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
