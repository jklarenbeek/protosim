"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
const index_js_1 = require("@modelcontextprotocol/sdk/server/index.js");
const stdio_js_1 = require("@modelcontextprotocol/sdk/server/stdio.js");
const types_js_1 = require("@modelcontextprotocol/sdk/types.js");
const child_process_1 = require("child_process");
const util_1 = require("util");
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const execAsync = (0, util_1.promisify)(child_process_1.exec);
// Create the MCP server instance
const server = new index_js_1.Server({
    name: "protosim-mcp",
    version: "1.0.0",
}, {
    capabilities: {
        tools: {},
    },
});
// Define tools
const initArduinoProjectTool = {
    name: "init_arduino_project",
    description: "Initializes a basic PlatformIO Arduino project for protosim",
    inputSchema: {
        type: "object",
        properties: {
            project_path: {
                type: "string",
                description: "Path to the directory to initialize the project in",
            },
            env_name: {
                type: "string",
                description: "Name of the PlatformIO environment (e.g., uno)",
                default: "uno",
            },
        },
        required: ["project_path"],
    },
};
const compileFirmwareTool = {
    name: "compile_firmware",
    description: "Compiles the firmware using PlatformIO and returns the path to the .elf file",
    inputSchema: {
        type: "object",
        properties: {
            project_path: {
                type: "string",
                description: "Path to the PlatformIO project directory",
            },
            env_name: {
                type: "string",
                description: "Name of the PlatformIO environment to compile",
                default: "uno",
            },
        },
        required: ["project_path"],
    },
};
const runSimulationTool = {
    name: "run_simulation",
    description: "Runs the compiled firmware in protosim with full cycle-exact profiling and debug capabilities",
    inputSchema: {
        type: "object",
        properties: {
            elf_path: {
                type: "string",
                description: "Path to the .elf file (usually in .pio/build/<env>/firmware.elf)",
            },
            max_steps: {
                type: "number",
                description: "Maximum number of steps to simulate. Essential to prevent infinite loops (e.g., 5000000).",
            },
            breakpoints: {
                type: "array",
                items: { type: "string" },
                description: "Array of function names or flash addresses to break on (e.g., ['loop', 'uart_tx'])",
            },
            watches: {
                type: "array",
                items: { type: "string" },
                description: "Array of watch strings (e.g., ['0x0100:1:state'])",
            },
            flags: {
                type: "array",
                items: { type: "string" },
                description: "Additional protosim flags (e.g., ['--coverage', '--profile', '--callgraph', '--dump-regs'])",
            },
        },
        required: ["elf_path", "max_steps"],
    },
};
server.setRequestHandler(types_js_1.ListToolsRequestSchema, async () => {
    return {
        tools: [initArduinoProjectTool, compileFirmwareTool, runSimulationTool],
    };
});
server.setRequestHandler(types_js_1.CallToolRequestSchema, async (request) => {
    try {
        if (request.params.name === "init_arduino_project") {
            const args = request.params.arguments;
            const envName = args.env_name || "uno";
            const fullPath = path.resolve(args.project_path);
            if (!fs.existsSync(fullPath)) {
                fs.mkdirSync(fullPath, { recursive: true });
            }
            const pioIni = `[env:${envName}]
platform = atmelavr
board = uno
framework = arduino
`;
            fs.writeFileSync(path.join(fullPath, "platformio.ini"), pioIni);
            const srcPath = path.join(fullPath, "src");
            if (!fs.existsSync(srcPath)) {
                fs.mkdirSync(srcPath);
            }
            const mainCpp = `#include <Arduino.h>

void setup() {
  Serial.begin(9600);
  Serial.println("Ready");
}

void loop() {
  delay(1000);
}
`;
            fs.writeFileSync(path.join(srcPath, "main.cpp"), mainCpp);
            return {
                content: [
                    {
                        type: "text",
                        text: `Initialized PlatformIO project at ${fullPath}. 
Environment: ${envName}
You can now add your code to ${path.join(fullPath, "src", "main.cpp")}`,
                    },
                ],
            };
        }
        if (request.params.name === "compile_firmware") {
            const args = request.params.arguments;
            const envName = args.env_name || "uno";
            const fullPath = path.resolve(args.project_path);
            try {
                const { stdout, stderr } = await execAsync(`pio run -e ${envName}`, { cwd: fullPath });
                const elfPath = path.join(fullPath, ".pio", "build", envName, "firmware.elf");
                return {
                    content: [
                        {
                            type: "text",
                            text: `Compilation successful.
ELF file located at: ${elfPath}

Compiler output:
${stdout}`,
                        },
                    ],
                };
            }
            catch (error) {
                return {
                    isError: true,
                    content: [
                        {
                            type: "text",
                            text: `Compilation failed:
${error.stdout}
${error.stderr}`,
                        },
                    ],
                };
            }
        }
        if (request.params.name === "run_simulation") {
            const args = request.params.arguments;
            const fullElfPath = path.resolve(args.elf_path);
            if (!fs.existsSync(fullElfPath)) {
                throw new Error(`ELF file not found at ${fullElfPath}`);
            }
            // Resolve protosim executable
            // First try local bin, then global
            let protosimCmd = "protosim";
            const localCmd = path.resolve(path.join(__dirname, "..", "..", "bin", "protosim"));
            if (fs.existsSync(localCmd)) {
                protosimCmd = localCmd;
            }
            const cmdArgs = [protosimCmd, fullElfPath, `--max-steps ${args.max_steps}`];
            if (args.breakpoints) {
                args.breakpoints.forEach((bp) => cmdArgs.push(`-b ${bp}`));
            }
            if (args.watches) {
                args.watches.forEach((w) => cmdArgs.push(`-w ${w}`));
            }
            if (args.flags) {
                args.flags.forEach((f) => cmdArgs.push(f));
            }
            const command = cmdArgs.join(" ");
            try {
                const { stdout, stderr } = await execAsync(command);
                // Simple heuristic to extract parsed info
                const breakpointsHit = {};
                const bpRegex = /\[DBG\]\s+BP\s+'([^']+)'\s+hit\s+(\d+)\s+time\(s\)/g;
                let match;
                while ((match = bpRegex.exec(stdout)) !== null) {
                    breakpointsHit[match[1]] = parseInt(match[2], 10);
                }
                const coverageRegex = /\[COV\]\s+TOTAL\s+\d+\s+\d+\s+([0-9.]+%)/;
                const coverageMatch = stdout.match(coverageRegex);
                const coverage = coverageMatch ? coverageMatch[1] : null;
                const hotPathRegex = /\\[CG\\]\\s+Hot Path.+:\\s*\\n\\[CG\\]\\s+(.+)\\n/;
                const hotPathMatch = stdout.match(hotPathRegex);
                const hotPath = hotPathMatch ? hotPathMatch[1].trim() : null;
                const parsedOutput = {
                    command_executed: command,
                    breakpoints_hit: breakpointsHit,
                    coverage_total: coverage,
                    hot_path: hotPath,
                    full_output: stdout // We provide the full output as well for deeper analysis
                };
                return {
                    content: [
                        {
                            type: "text",
                            text: JSON.stringify(parsedOutput, null, 2),
                        },
                    ],
                };
            }
            catch (error) {
                // Return stdout/stderr even on crash (since max-steps might exit with a code)
                return {
                    content: [
                        {
                            type: "text",
                            text: `Execution completed or errored:
${error.stdout}
${error.stderr}`,
                        },
                    ],
                };
            }
        }
        throw new Error(`Unknown tool: ${request.params.name}`);
    }
    catch (error) {
        return {
            isError: true,
            content: [
                {
                    type: "text",
                    text: `Error executing \${request.params.name}: \${error.message}`,
                },
            ],
        };
    }
});
// Run the server
async function run() {
    const transport = new stdio_js_1.StdioServerTransport();
    await server.connect(transport);
    console.error("Protosim MCP Server is running on stdio");
}
run().catch((error) => {
    console.error("Failed to start server:", error);
    process.exit(1);
});
//# sourceMappingURL=index.js.map