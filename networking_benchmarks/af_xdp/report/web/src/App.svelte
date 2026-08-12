<script>
  import { onMount, onDestroy } from 'svelte';
  import { mountTopology2D } from './lib/2d/index.js';
  import { mountTopology3D } from './lib/topology3d.js';

  let container;
  let fleet = null;
  let error = '';
  let loading = true;
  let mode = '2d';
  let handle = null;

  function remount() {
    if (handle) handle.dispose();
    container.innerHTML = '';
    handle = (mode === '2d' ? mountTopology2D : mountTopology3D)(container, fleet);
  }
  function setMode(m) {
    if (m !== mode && fleet) { mode = m; remount(); }
  }

  onMount(async () => {
    try {
      // Data source: ?data=<url> (e.g. a results dir's fleet.json), else bundled sample.
      const params = new URLSearchParams(location.search);
      const url = params.get('data') || 'fleet.json';
      const res = await fetch(url);
      if (!res.ok) throw new Error(`GET ${url} -> ${res.status}`);
      fleet = await res.json();
      loading = false;
      remount();
    } catch (e) {
      loading = false;
      error = e.message || String(e);
    }
  });

  onDestroy(() => { if (handle) handle.dispose(); });
</script>

<div class="toolbar">
  <button class:active={mode === '2d'} on:click={() => setMode('2d')}>2D</button>
  <button class:active={mode === '3d'} on:click={() => setMode('3d')}>3D</button>
</div>

<div class="root" bind:this={container}></div>

{#if loading}<div class="msg">Loading topology…</div>{/if}
{#if error}<div class="msg err">Failed to load topology: {error}<br /><small>Pass ?data=&lt;url-to-fleet.json&gt; or place fleet.json alongside index.html.</small></div>{/if}

<style>
  .root { position: fixed; inset: 0; }
  .toolbar { position: fixed; top: 16px; left: 50%; transform: translateX(-50%); z-index: 1500;
    display: flex; gap: 4px; background: rgba(22,27,34,.9); border: 1px solid #30363d;
    border-radius: 8px; padding: 4px; backdrop-filter: blur(8px); }
  .toolbar button { background: transparent; color: #8b949e; border: none; border-radius: 6px;
    padding: 5px 16px; font: 600 13px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; cursor: pointer; }
  .toolbar button.active { background: rgba(88,166,255,.18); color: #58a6ff; }
  .msg { position: fixed; bottom: 16px; right: 16px; z-index: 2000; color: #e6edf3;
    background: rgba(22,27,34,.92); border: 1px solid #30363d; border-radius: 6px; padding: 8px 12px;
    font: 13px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }
  .msg.err { color: #f85149; border-color: #f85149; }
  .msg small { color: #8b949e; }
</style>
