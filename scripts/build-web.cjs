const fs = require('node:fs');
const path = require('node:path');
const esbuild = require('esbuild');

async function build() {
  const root = path.resolve(__dirname, '..');
  const output = path.join(root, 'www');
  fs.mkdirSync(output, { recursive: true });
  fs.copyFileSync(path.join(root, 'static/index.html'), path.join(output, 'index.html'));
  fs.copyFileSync(path.join(root, 'static/server-config.js'), path.join(output, 'server-config.js'));
  await esbuild.build({
    entryPoints: [path.join(root, 'static/map.js'), path.join(root, 'static/token-session.js')],
    bundle: true,
    format: 'iife',
    target: 'es2020',
    outdir: output,
    define: { __PUSH_ENABLED__: String(fs.existsSync(path.join(root, 'android/app/google-services.json'))) },
  });
  console.log('Built Android web assets. No authentication tokens are bundled.');
}
build().catch(error => { console.error(error.message); process.exit(1); });
