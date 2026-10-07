import { McpServer } from '@modelcontextprotocol/server';
import { serveStdio } from '@modelcontextprotocol/server/stdio';
import * as net from 'node:net';
import * as path from 'node:path';
import * as os from 'node:os';
import * as z from 'zod/v4';

interface RpcResponse {
  id: number;
  ok: boolean;
  result?: unknown;
  error?: string;
}

function socketPath(): string {
  const runtime = process.env.XDG_RUNTIME_DIR;
  return path.join(runtime && runtime.length > 0 ? runtime : os.tmpdir(), 'docsuite-device-core.sock');
}

function rpc(method: string, params: Record<string, unknown> = {}): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const id = Date.now();
    const socket = net.createConnection({ path: socketPath() });
    let buffer = '';

    const finish = (error?: Error, result?: unknown) => {
      socket.removeAllListeners();
      socket.destroy();
      if (error) reject(error);
      else resolve(result);
    };

    socket.setTimeout(15000, () => finish(new Error('DocSuite device service timed out')));
    socket.on('error', error => finish(error));
    socket.on('connect', () => {
      socket.write(JSON.stringify({ id, method, params }) + '\n');
    });
    socket.on('data', chunk => {
      buffer += chunk.toString('utf8');
      const newline = buffer.indexOf('\n');
      if (newline < 0) return;

      try {
        const response = JSON.parse(buffer.slice(0, newline)) as RpcResponse;
        if (!response.ok) {
          finish(new Error(response.error ?? 'DocSuite device service request failed'));
          return;
        }
        finish(undefined, response.result);
      } catch (error) {
        finish(error instanceof Error ? error : new Error(String(error)));
      }
    });
  });
}

function textResult(value: unknown) {
  return {
    content: [{ type: 'text' as const, text: JSON.stringify(value, null, 2) }]
  };
}

function createServer(): McpServer {
  const server = new McpServer({ name: 'docsuite-device-core', version: '0.4.0' });

  server.registerTool(
    'device_list',
    {
      description: 'List printers and scanners visible to the local DocSuite Device Core service.',
      inputSchema: z.object({})
    },
    async () => textResult(await rpc('device.list'))
  );

  server.registerTool(
    'printer_status',
    {
      description: 'Read the current physical printer state, warnings and supply levels.',
      inputSchema: z.object({ printer: z.string().min(1) })
    },
    async ({ printer }) => textResult(await rpc('printer.status', { printer }))
  );

  server.registerTool(
    'printer_capabilities',
    {
      description: 'Read normalized physical printer capabilities. Refresh bypasses the service cache.',
      inputSchema: z.object({
        printer: z.string().min(1),
        refresh: z.boolean().optional().default(false)
      })
    },
    async ({ printer, refresh }) =>
      textResult(await rpc('printer.capabilities', { printer, refresh }))
  );

  server.registerTool(
    'printer_jobs',
    {
      description: 'List CUPS jobs for a printer.',
      inputSchema: z.object({
        printer: z.string().min(1),
        includeCompleted: z.boolean().optional().default(true)
      })
    },
    async ({ printer, includeCompleted }) =>
      textResult(await rpc('printer.jobs', {
        printer,
        include_completed: includeCompleted
      }))
  );

  server.registerTool(
    'scanner_capabilities',
    {
      description: 'Read normalized SANE capabilities for a discovered scanner.',
      inputSchema: z.object({ scanner: z.string().min(1) })
    },
    async ({ scanner }) => textResult(await rpc('scanner.capabilities', { scanner }))
  );

  return server;
}

void serveStdio(createServer);
console.error('DocSuite Device MCP sidecar running on stdio');
