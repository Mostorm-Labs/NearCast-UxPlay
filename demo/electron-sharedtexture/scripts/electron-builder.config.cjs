const fs = require('fs');
const path = require('path');

const demoRoot = path.resolve(__dirname, '..');
const repoRoot = path.resolve(demoRoot, '..', '..');
const buildRoot = path.resolve(process.env.UXPLAY_BUILD_ROOT || path.join(repoRoot, 'build'));
const msysRoot = path.resolve(process.env.MSYS2_ROOT || 'D:\\msys64');
const bonjourSdkSetup = path.join(repoRoot, 'bonjoursdksetup.exe');

function requirePath(label, targetPath) {
  if (!fs.existsSync(targetPath)) {
    throw new Error(`${label} not found: ${targetPath}`);
  }
  return targetPath;
}

const extraResources = [
  {
    from: requirePath('UxPlay executable', path.join(buildRoot, 'uxplay.exe')),
    to: 'uxplay-runtime/uxplay.exe',
  },
  {
    from: buildRoot,
    to: 'uxplay-runtime',
    filter: ['*.dll'],
  },
  {
    from: requirePath('GStreamer plugin directory', path.join(buildRoot, 'lib', 'gstreamer-1.0')),
    to: 'uxplay-runtime/lib/gstreamer-1.0',
  },
  {
    from: requirePath('Bonjour SDK installer', bonjourSdkSetup),
    to: 'third-party/bonjour/bonjoursdksetup.exe',
  },
];

const scannerPath = path.join(
  msysRoot,
  'mingw64',
  'libexec',
  'gstreamer-1.0',
  'gst-plugin-scanner.exe',
);

if (fs.existsSync(scannerPath)) {
  extraResources.push({
    from: scannerPath,
    to: 'uxplay-runtime/libexec/gstreamer-1.0/gst-plugin-scanner.exe',
  });
}

module.exports = {
  appId: 'com.uxplay.sharedtexture.demo',
  productName: 'UxPlaySharedTexture',
  copyright: 'Copyright (C) 2026 UxPlay contributors',
  directories: {
    output: 'dist/installer',
  },
  files: [
    'package.json',
    'main.js',
    'index.html',
    'pin.html',
    'pin-renderer.js',
    'renderer.js',
    'README.md',
    'DEMO_INTRODUCTION.md',
    'WEBSOCKET_PROTOCOL.md',
    'websocket_protocol_advance.md',
  ],
  extraResources,
  npmRebuild: false,
  asar: true,
  win: {
    target: [
      {
        target: 'nsis',
        arch: ['x64'],
      },
    ],
    artifactName: '${productName}-Setup-${version}.${ext}',
  },
  nsis: {
    oneClick: false,
    perMachine: false,
    allowElevation: true,
    allowToChangeInstallationDirectory: true,
    createDesktopShortcut: true,
    createStartMenuShortcut: true,
    include: 'scripts/installer.nsh',
    shortcutName: 'UxPlay SharedTexture',
    uninstallDisplayName: 'UxPlay SharedTexture',
  },
};
