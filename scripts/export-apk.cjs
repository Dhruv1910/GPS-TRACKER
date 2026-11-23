'use strict';
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const source = path.join(root, 'android/app/build/outputs/apk/debug/app-debug.apk');
const destination = path.join(root, 'releases/field-tracker.apk');
fs.mkdirSync(path.dirname(destination), { recursive: true });
fs.copyFileSync(source, destination);
console.log('APK ready: releases/field-tracker.apk');
