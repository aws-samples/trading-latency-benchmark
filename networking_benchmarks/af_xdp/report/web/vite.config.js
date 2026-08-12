import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';

// base: './' so a built bundle works when opened from any path.
export default defineConfig({
  base: './',
  plugins: [svelte()],
});
