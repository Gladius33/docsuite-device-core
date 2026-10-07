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

    socket.setTimeout(30000, () => finish(new Error('DocSuite device service timed out')));
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
  const server = new McpServer({ name: 'docsuite-device-core', version: '0.5.0' });

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
    'printer_preflight',
    {
      description: 'Validate standard print settings without submitting a print job. Detailed mode additionally asks CUPS to evaluate cross-option constraints and can be slower.',
      inputSchema: z.object({
        printer: z.string().min(1).max(1024),
        media: z.string().min(1).max(256).optional().default('iso_a4_210x297mm'),
        mediaSource: z.string().max(256).optional().default(''),
        mediaType: z.string().max(256).optional().default(''),
        colorMode: z.string().min(1).max(256).optional().default('color'),
        sides: z.string().min(1).max(256).optional().default('one-sided'),
        quality: z.number().int().min(0).max(100).optional().default(4),
        copies: z.number().int().min(1).max(9999).optional().default(1),
        detailed: z.boolean().optional().default(false),
        refresh: z.boolean().optional().default(false)
      })
    },
    async ({
      printer,
      media,
      mediaSource,
      mediaType,
      colorMode,
      sides,
      quality,
      copies,
      detailed,
      refresh
    }) => textResult(await rpc('printer.preflight', {
      printer,
      media,
      media_source: mediaSource,
      media_type: mediaType,
      color_mode: colorMode,
      sides,
      quality,
      copies,
      detailed,
      refresh
    }))
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
    'printer_cancel_job',
    {
      description: 'Cancel a CUPS job only if the local DocSuite service confirms that the job belongs to the current Unix user.',
      inputSchema: z.object({
        printer: z.string().min(1),
        jobId: z.number().int().positive()
      })
    },
    async ({ printer, jobId }) =>
      textResult(await rpc('printer.cancel_job', {
        printer,
        job_id: jobId
      }))
  );

  server.registerTool(
    'scanner_capabilities',
    {
      description: 'Read normalized scanner capabilities through the DocSuite SANE/eSCL router.',
      inputSchema: z.object({ scanner: z.string().min(1) })
    },
    async ({ scanner }) => textResult(await rpc('scanner.capabilities', { scanner }))
  );

  server.registerTool(
    'scanner_scan',
    {
      description: 'Acquire one page from a local scanner. The service writes the result only into its private runtime scan directory and returns the generated path.',
      inputSchema: z.object({
        scanner: z.string().min(1),
        dpi: z.number().int().min(75).max(1200).optional().default(300),
        mode: z.enum(['Color', 'Gray', 'Lineart']).optional().default('Color'),
        source: z.string().min(1).max(128).optional().default('Flatbed')
      })
    },
    async ({ scanner, dpi, mode, source }) =>
      textResult(await rpc('scanner.scan', { scanner, dpi, mode, source }))
  );

  return server;
}

void serveStdio(createServer);
console.error('DocSuite Device MCP sidecar running on stdio');
