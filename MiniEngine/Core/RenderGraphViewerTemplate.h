#pragma once

#include <string_view>

namespace RenderGraph::Debug::Viewer
{
    inline constexpr std::string_view kHtmlPrefix = R"RGHTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Render Graph Preview</title>
<style>
:root {
    color-scheme: light dark;
    --bg: #f7f8fa;
    --panel: #fff;
    --fg: #18212f;
    --muted: #667085;
    --line: #98a2b3;
    --data: #2563eb;
    --hazard: #d97706;
    --live: #dbeafe;
    --dead: #e5e7eb;
    --root: #7c3aed;
    --error: #b42318;
}
@media (prefers-color-scheme: dark) {
    :root {
        --bg: #101318;
        --panel: #171b22;
        --fg: #e5e7eb;
        --muted: #98a2b3;
        --line: #667085;
        --data: #60a5fa;
        --hazard: #fbbf24;
        --live: #1e3a5f;
        --dead: #30343b;
        --root: #c4b5fd;
        --error: #fda29b;
    }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 14px/1.45 "Segoe UI", system-ui, sans-serif; }
header, main { max-width: 1500px; margin: auto; padding: 20px; }
header { display: flex; gap: 24px; align-items: flex-end; justify-content: space-between; flex-wrap: wrap; }
h1, h2 { margin: 0 0 12px; font-weight: 600; }
h1 { font-size: 24px; }
h2 { font-size: 17px; }
.summary, .controls { display: flex; gap: 16px; flex-wrap: wrap; }
.stat b { display: block; font-size: 20px; }
.stat span, .muted { color: var(--muted); }
.controls label { display: flex; gap: 7px; align-items: center; }
section { background: var(--panel); border: 1px solid color-mix(in srgb, var(--line) 35%, transparent); border-radius: 10px; margin-bottom: 18px; padding: 16px; }
.graph-wrap, .table-wrap { overflow: auto; }
svg { display: block; min-width: 100%; background: var(--panel); }
.edge { fill: none; stroke-width: 1.5; }
.edge.data { stroke: var(--data); }
.edge.hazard { stroke: var(--hazard); stroke-dasharray: 6 5; }
.pass rect { stroke: var(--line); stroke-width: 1.5; fill: var(--live); rx: 7; }
.pass.culled rect { fill: var(--dead); stroke-dasharray: 5 4; }
.pass.unscheduled rect { fill: var(--dead); stroke-dasharray: 2 4; }
.pass.root rect { stroke: var(--root); stroke-width: 3; }
.pass text { fill: var(--fg); font-size: 12px; pointer-events: none; }
.pass { cursor: pointer; }
.detail { min-height: 22px; margin-top: 10px; color: var(--muted); }
table { border-collapse: collapse; white-space: nowrap; width: 100%; }
th, td { border-bottom: 1px solid color-mix(in srgb, var(--line) 35%, transparent); padding: 7px 9px; text-align: center; }
th:first-child, td:first-child { text-align: left; position: sticky; left: 0; background: var(--panel); }
td.alive { background: color-mix(in srgb, var(--data) 12%, transparent); }
td.use { color: var(--data); font-weight: 600; }
.diagnostic { padding: 7px 0; }
.diagnostic.error { color: var(--error); }
code { font-family: "Cascadia Mono", Consolas, monospace; }
</style>
</head>
<body>
<header>
  <div><h1 id="title"></h1><div class="muted" id="state"></div></div>
  <div class="summary" id="summary"></div>
  <div class="controls">
    <label><input id="show-culled" type="checkbox" checked> Culled passes</label>
    <label><input id="show-hazards" type="checkbox" checked> Hazard edges</label>
  </div>
</header>
<main>
  <section>
    <h2>Pass DAG</h2>
    <div class="graph-wrap"><svg id="dag" role="img" aria-label="Render graph pass dependency DAG"></svg></div>
    <div id="detail" class="detail">Select a pass to inspect its declarations.</div>
  </section>
  <section><h2>Resource lifetime matrix</h2><div class="table-wrap"><table id="lifetimes"></table></div></section>
  <section><h2>Barrier plan</h2><div class="table-wrap"><table id="barriers"></table></div></section>
  <section><h2>Diagnostics</h2><div id="diagnostics"></div></section>
</main>
<script id="render-graph-data" type="application/json">)RGHTML";

    inline constexpr std::string_view kHtmlSuffix = R"RGHTML(</script>
<script>
const data = JSON.parse(document.getElementById('render-graph-data').textContent);
const ns = 'http://www.w3.org/2000/svg';
const esc = value => String(value).replace(
    /[&<>"']/g,
    character => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[character]));

const byId = id => data.passes.find(pass => pass.id === id);
const resName = id => {
    const resource = data.resources.find(item => item.resource === id);
    return resource ? resource.name : `resource ${id}`;
};

document.getElementById('title').textContent = data.graphName;
document.title = `${data.graphName} - Render Graph`;
document.getElementById('state').textContent = `${data.compileState} | epoch ${data.epoch}`;

const stats = [
    ['Live passes', data.result.livePassCount],
    ['Culled', data.result.culledPassCount],
    ['Barriers', data.result.barrierCount],
    ['Memory slots', data.result.memorySlotCount]
];
document.getElementById('summary').innerHTML = stats
    .map(([key, value]) => `<div class="stat"><b>${value}</b><span>${key}</span></div>`)
    .join('');

function svgElement(name, attributes = {}) {
    const element = document.createElementNS(ns, name);
    Object.entries(attributes).forEach(([key, value]) => element.setAttribute(key, value));
    return element;
}

const isCulled = pass => data.compileState === 'Succeeded' && !pass.live;
const passStatus = pass => pass.executionIndex !== null
    ? `exec ${pass.executionIndex}`
    : (isCulled(pass) ? 'culled' : 'not scheduled');

function renderDag() {
    const showCulled = document.getElementById('show-culled').checked;
    const showHazards = document.getElementById('show-hazards').checked;
    const passes = data.passes.filter(pass => showCulled || !isCulled(pass));
    const passIds = new Set(passes.map(pass => pass.id));
    const edges = data.edges.filter(edge =>
        passIds.has(edge.from) &&
        passIds.has(edge.to) &&
        (showHazards || edge.kind === 'Data'));

    const activeIds = new Set(
        passes.filter(pass => pass.executionIndex !== null).map(pass => pass.id));
    const incoming = new Map();
    for (const edge of edges.filter(edge => activeIds.has(edge.from) && activeIds.has(edge.to))) {
        if (!incoming.has(edge.to))
            incoming.set(edge.to, []);
        incoming.get(edge.to).push(edge);
    }

    const live = [...passes]
        .filter(pass => pass.executionIndex !== null)
        .sort((left, right) => left.executionIndex - right.executionIndex);
    const depth = new Map(passes.map(pass => [pass.id, 0]));
    for (const pass of live) {
        for (const edge of incoming.get(pass.id) || []) {
            depth.set(
                pass.id,
                Math.max(depth.get(pass.id) || 0, (depth.get(edge.from) || 0) + 1));
        }
    }

    const maxLiveDepth = Math.max(0, ...live.map(pass => depth.get(pass.id) || 0));
    const inactiveDepth = maxLiveDepth + 1;
    for (const pass of passes.filter(pass => pass.executionIndex === null))
        depth.set(pass.id, inactiveDepth);

    const rows = new Map();
    const positions = new Map();
    for (const pass of passes) {
        const column = depth.get(pass.id) || 0;
        const row = rows.get(column) || 0;
        rows.set(column, row + 1);
        positions.set(pass.id, {x: 35 + column * 210, y: 35 + row * 90});
    }

    const width = Math.max(700, (Math.max(0, ...depth.values()) + 1) * 210 + 80);
    const height = Math.max(220, Math.max(1, ...rows.values()) * 90 + 80);
    const svg = document.getElementById('dag');
    svg.replaceChildren();
    svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    svg.setAttribute('width', width);
    svg.setAttribute('height', height);

    const definitions = svgElement('defs');
    for (const [id, color] of [
        ['arrow-data', 'var(--data)'],
        ['arrow-hazard', 'var(--hazard)']
    ]) {
        const marker = svgElement('marker', {
            id,
            viewBox: '0 0 10 10',
            refX: 9,
            refY: 5,
            markerWidth: 6,
            markerHeight: 6,
            orient: 'auto-start-reverse'
        });
        marker.append(svgElement('path', {d: 'M 0 0 L 10 5 L 0 10 z', fill: color}));
        definitions.append(marker);
    }
    svg.append(definitions);

    const edgeGroups = new Map();
    for (const edge of edges) {
        const key = `${edge.from}:${edge.to}`;
        if (!edgeGroups.has(key))
            edgeGroups.set(key, {from: edge.from, to: edge.to, items: []});
        edgeGroups.get(key).items.push(edge);
    }

    for (const group of edgeGroups.values()) {
        const from = positions.get(group.from);
        const to = positions.get(group.to);
        if (!from || !to)
            continue;

        const kind = group.items.some(edge => edge.kind === 'Data') ? 'data' : 'hazard';
        const path = svgElement('path', {
            d: `M ${from.x + 150} ${from.y + 25} C ${from.x + 175} ${from.y + 25}, ${to.x - 25} ${to.y + 25}, ${to.x} ${to.y + 25}`,
            class: `edge ${kind}`,
            'marker-end': `url(#arrow-${kind})`
        });
        const title = svgElement('title');
        title.textContent = group.items
            .map(edge => `${edge.kind}: ${resName(edge.resource)} v${edge.version}`)
            .join(' · ');
        path.append(title);
        svg.append(path);
    }

    for (const pass of passes) {
        const position = positions.get(pass.id);
        const state = isCulled(pass)
            ? 'culled'
            : (pass.executionIndex === null ? 'unscheduled' : '');
        const group = svgElement('g', {
            class: `pass ${state} ${pass.root ? 'root' : ''}`
        });
        group.append(svgElement('rect', {
            x: position.x,
            y: position.y,
            width: 150,
            height: 50
        }));

        const title = svgElement('title');
        title.textContent = pass.name;
        group.append(title);

        const name = svgElement('text', {x: position.x + 10, y: position.y + 22});
        name.textContent = pass.name.length > 24 ? `${pass.name.slice(0, 23)}...` : pass.name;
        group.append(name);

        const status = svgElement('text', {x: position.x + 10, y: position.y + 40});
        status.textContent = passStatus(pass);
        group.append(status);
        group.addEventListener('click', () => {
            const accesses = pass.accesses.map(access =>
                `${access.mode} ${resName(access.resource)} v${access.inputVersion}` +
                `${access.outputVersion !== access.inputVersion ? `→v${access.outputVersion}` : ''}` +
                ` as ${access.state.usage}`)
                .join(' · ');
            document.getElementById('detail').textContent =
                `${pass.name}: ${accesses || 'no resource declarations'}` +
                `${pass.flags !== 'None' ? ` · ${pass.flags}` : ''}`;
        });
        svg.append(group);
    }
}

function renderLifetimes() {
    const passes = data.passes
        .filter(pass => pass.executionIndex !== null)
        .sort((left, right) => left.executionIndex - right.executionIndex);
    const epilogue = passes.length;
    let html = '<thead><tr><th>Resource version</th>' +
        passes.map(pass => `<th title="${esc(pass.name)}">${pass.executionIndex}</th>`).join('') +
        '<th title="Graph epilogue">E</th></tr></thead><tbody>';

    const resources = data.compileState === 'Succeeded'
        ? data.resources.filter(resource =>
            resource.firstUse !== null || resource.imported || resource.exported)
        : data.resources;

    for (const resource of resources) {
        const slot = resource.memorySlot !== null
            ? ` · memory slot ${resource.memorySlot}`
            : '';
        html += `<tr><td title="lifetime ${resource.firstUse ?? '-'}..${resource.lastUse ?? '-'}">` +
            `${esc(resource.name)} v${resource.version}${slot}</td>`;

        for (const pass of passes) {
            const marks = [];
            for (const access of pass.accesses.filter(item => item.resource === resource.resource)) {
                if (access.mode === 'Read' && access.inputVersion === resource.version)
                    marks.push('R');
                if (access.mode === 'Write' && access.outputVersion === resource.version)
                    marks.push('W');
                if (access.mode === 'ReadWrite') {
                    if (access.inputVersion === resource.version)
                        marks.push('R');
                    if (access.outputVersion === resource.version)
                        marks.push('RW');
                }
            }
            const alive = resource.firstUse !== null &&
                pass.executionIndex >= resource.firstUse &&
                pass.executionIndex <= resource.lastUse;
            html += `<td class="${alive ? 'alive ' : ''}${marks.length ? 'use' : ''}">` +
                `${marks.join('/')}</td>`;
        }

        const epilogueAlive = resource.firstUse !== null && resource.lastUse === epilogue;
        html += `<td class="${epilogueAlive ? 'alive' : ''}">` +
            `${resource.exported ? 'Export' : ''}</td></tr>`;
    }
    document.getElementById('lifetimes').innerHTML = html + '</tbody>';
}

function renderBarriers() {
    let html = '<thead><tr><th>Kind</th><th>Pass</th><th>Resource</th>' +
        '<th>Before</th><th>After</th><th>Placement</th></tr></thead><tbody>';
    for (const barrier of data.barriers) {
        const pass = barrier.pass === null ? null : byId(barrier.pass);
        const state = value =>
            `${value.usage}${value.stages !== 'None' ? ` (${value.stages})` : ''}`;
        const aliasing = barrier.kind === 'Aliasing';
        const before = aliasing
            ? (barrier.aliasedResourceBefore === null
                ? 'any prior owner'
                : resName(barrier.aliasedResourceBefore))
            : state(barrier.before);
        const after = aliasing ? resName(barrier.resource) : state(barrier.after);
        html += `<tr><td>${esc(barrier.kind)}</td>` +
            `<td>${esc(pass ? pass.name : 'Graph epilogue')}</td>` +
            `<td>${esc(resName(barrier.resource))} v${barrier.version}</td>` +
            `<td>${esc(before)}</td>` +
            `<td>${esc(after)}</td>` +
            `<td>${barrier.afterPass ? 'after pass' : 'before pass'}</td></tr>`;
    }
    if (!data.barriers.length)
        html += '<tr><td colspan="6" class="muted">No barriers.</td></tr>';
    document.getElementById('barriers').innerHTML = html + '</tbody>';
}

function renderDiagnostics() {
    const root = document.getElementById('diagnostics');
    if (!data.diagnostics.length) {
        root.innerHTML = '<span class="muted">No diagnostics.</span>';
        return;
    }
    root.innerHTML = data.diagnostics
        .map(diagnostic =>
            `<div class="diagnostic ${diagnostic.severity.toLowerCase()}">` +
            `<code>${esc(diagnostic.code)}</code> — ${esc(diagnostic.message)}</div>`)
        .join('');
}

document.getElementById('show-culled').addEventListener('change', renderDag);
document.getElementById('show-hazards').addEventListener('change', renderDag);
renderDag();
renderLifetimes();
renderBarriers();
renderDiagnostics();
</script>
</body>
</html>
)RGHTML";
} // namespace RenderGraph::Debug::Viewer
