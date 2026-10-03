// Generate the browser client from the copied Motrix sources (Node 24+).
import { stripTypeScriptTypes } from 'node:module';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const root = new URL('../../', import.meta.url);
const files = ['aria2/aria2-pause-state.ts', 'aria2/json-rpc-protocol.ts', 'aria2/aria2-rpc-client.ts', 'format-bytes.ts'];
const body = files.map(file => {
  let source = readFileSync(new URL('third_party/motrix/' + file, root), 'utf8');
  source = source.replace(/import\s+(?:type\s+)?[\s\S]*?from\s+['"][^'"]+['"]\s*/g, '');
  return stripTypeScriptTypes(source, { mode: 'transform' }).replace(/\bexport /g, '');
}).join('\n');
writeFileSync(fileURLToPath(new URL('resources/vui/motrix-client.js', root)),
  '// Generated from third_party/motrix by tools/download-engine/generate-client.mjs.\n// Copyright 2018-present Dr_rOot. MIT; see licenses/Motrix-LICENSE.txt.\n(() => {\nconst DEFAULT_BYTE_UNIT_SYSTEM = "decimal";\n' + body + '\nwindow.Motrix = { Aria2RpcClient, JsonRpcProtocol, formatBytes, formatSpeed };\n})();\n');
