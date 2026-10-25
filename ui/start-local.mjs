import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const windows = process.platform === 'win32';
const command = windows ? 'powershell.exe' : 'bash';
const args = windows
  ? ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', fileURLToPath(new URL('./start-local.ps1', import.meta.url))]
  : [fileURLToPath(new URL('./start-local.sh', import.meta.url))];

const child = spawn(command, args, { cwd: root, stdio: 'inherit', windowsHide: true });
child.on('error', error => {
  console.error(`Could not start Romeo: ${error.message}`);
  process.exitCode = 1;
});
child.on('exit', (code, signal) => { process.exitCode = code ?? (signal ? 1 : 0); });
// Keep npm's foreground process alive until its child exits after Ctrl+C.
process.on('SIGINT', () => { if (!windows) child.kill('SIGINT'); });
process.on('SIGTERM', () => { child.kill('SIGTERM'); });
