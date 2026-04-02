import { defineConfig } from 'vite';

export default defineConfig({
  base: '/',
  build: {
    emptyOutDir: true,
    outDir: '../main/web_dist',
    assetsDir: 'assets',
    rollupOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: (assetInfo) => {
          if (assetInfo.name && assetInfo.name.endsWith('.css')) {
            return 'assets/app.css';
          }

          return 'assets/[name][extname]';
        }
      }
    }
  }
});
