// main.js
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const fs = require('fs').promises;
const path = require('path');
const os = require('os');
const cluster = require('cluster');

const CHUNK_SIZE = 64 * 1024; // 64KB chunks for better performance
const NUM_WORKERS = os.cpus().length; // Use all CPU cores
const BUFFER_SIZE = 128 * 1024; // 128KB buffer for file operations

class PerformantMP4Converter {
    async mkvToBinary(inputPath, outputPath) {
        console.log('Converting MP4 to binary...');
        const startTime = process.hrtime.bigint();
        
        const stats = await fs.stat(inputPath);
        const fileSize = stats.size;
        console.log(`Processing MP4 file: ${fileSize} bytes`);

        const inputHandle = await fs.open(inputPath, 'r');
        const outputHandle = await fs.open(outputPath, 'w');
        
        try {
            await this.processFileWithWorkers(
                inputHandle, 
                outputHandle, 
                fileSize, 
                'mp4-to-binary'
            );
        } finally {
            await inputHandle.close();
            await outputHandle.close();
        }

        const endTime = process.hrtime.bigint();
        const duration = Number(endTime - startTime) / 1e9;
        console.log(`✅ Done! Binary string saved to ${outputPath}`);
        console.log(`MP4 to binary conversion took: ${duration.toFixed(4)} seconds`);
    }

    async binaryToMp4(inputPath, outputPath) {
        console.log('Converting binary to MP4...');
        const startTime = process.hrtime.bigint();
        
        const stats = await fs.stat(inputPath);
        const fileSize = stats.size;
        console.log(`Processing binary file: ${fileSize} bytes`);

        const inputHandle = await fs.open(inputPath, 'r');
        const outputHandle = await fs.open(outputPath, 'w');
        
        try {
            await this.processFileWithWorkers(
                inputHandle, 
                outputHandle, 
                fileSize, 
                'binary-to-mp4'
            );
        } finally {
            await inputHandle.close();
            await outputHandle.close();
        }

        const endTime = process.hrtime.bigint();
        const duration = Number(endTime - startTime) / 1e9;
        console.log(`✅ Done! MP4 saved as ${outputPath}`);
        console.log(`Binary to MP4 conversion took: ${duration.toFixed(4)} seconds`);
    }

    async processFileWithWorkers(inputHandle, outputHandle, fileSize, operation) {
        const workers = [];
        const workQueue = [];
        const results = new Map();
        
        // Create chunks for processing
        let chunkIndex = 0;
        for (let offset = 0; offset < fileSize; offset += CHUNK_SIZE) {
            const size = Math.min(CHUNK_SIZE, fileSize - offset);
            workQueue.push({ 
                chunkIndex: chunkIndex++, 
                offset, 
                size 
            });
        }

        const totalChunks = workQueue.length;
        let processedChunks = 0;
        let outputOffset = 0;

        // Create workers
        for (let i = 0; i < NUM_WORKERS; i++) {
            const worker = new Worker(__filename, {
                workerData: { operation }
            });
            
            worker.on('message', async ({ chunkIndex, data, error }) => {
                if (error) {
                    throw new Error(error);
                }
                
                results.set(chunkIndex, data);
                processedChunks++;
                
                // Write results in order
                while (results.has(outputOffset / (operation === 'mp4-to-binary' ? CHUNK_SIZE : CHUNK_SIZE * 8))) {
                    const nextChunkIndex = Math.floor(outputOffset / (operation === 'mp4-to-binary' ? CHUNK_SIZE : CHUNK_SIZE * 8));
                    const chunkData = results.get(nextChunkIndex);
                    
                    if (chunkData) {
                        await outputHandle.write(Buffer.from(chunkData), 0, chunkData.length, outputOffset);
                        outputOffset += chunkData.length;
                        results.delete(nextChunkIndex);
                    } else {
                        break;
                    }
                }
                
                // Progress indicator
                if (processedChunks % Math.max(1, Math.floor(totalChunks / 20)) === 0) {
                    const progress = (processedChunks / totalChunks * 100).toFixed(1);
                    console.log(`Progress: ${progress}%`);
                }
                
                // Assign new work
                if (workQueue.length > 0) {
                    const work = workQueue.shift();
                    const buffer = Buffer.allocUnsafe(work.size);
                    const { bytesRead } = await inputHandle.read(buffer, 0, work.size, work.offset);
                    
                    worker.postMessage({
                        chunkIndex: work.chunkIndex,
                        data: buffer.subarray(0, bytesRead)
                    });
                } else {
                    worker.terminate();
                }
            });
            
            workers.push(worker);
        }

        // Distribute initial work
        for (let i = 0; i < Math.min(NUM_WORKERS, workQueue.length); i++) {
            const work = workQueue.shift();
            const buffer = Buffer.allocUnsafe(work.size);
            const { bytesRead } = await inputHandle.read(buffer, 0, work.size, work.offset);
            
            workers[i].postMessage({
                chunkIndex: work.chunkIndex,
                data: buffer.subarray(0, bytesRead)
            });
        }

        // Wait for all workers to finish
        return new Promise((resolve, reject) => {
            let finishedWorkers = 0;
            
            workers.forEach(worker => {
                worker.on('exit', () => {
                    finishedWorkers++;
                    if (finishedWorkers === workers.length) {
                        resolve();
                    }
                });
                
                worker.on('error', reject);
            });
        });
    }
}

// Worker thread logic
if (!isMainThread) {
    const { operation } = workerData;
    
    parentPort.on('message', ({ chunkIndex, data }) => {
        try {
            let result;
            
            if (operation === 'mp4-to-binary') {
                // Convert bytes to binary string with optimized approach
                const binaryArray = new Array(data.length * 8);
                let index = 0;
                
                for (let i = 0; i < data.length; i++) {
                    const byte = data[i];
                    // Unroll the loop for better performance
                    binaryArray[index++] = (byte >> 7) & 1;
                    binaryArray[index++] = (byte >> 6) & 1;
                    binaryArray[index++] = (byte >> 5) & 1;
                    binaryArray[index++] = (byte >> 4) & 1;
                    binaryArray[index++] = (byte >> 3) & 1;
                    binaryArray[index++] = (byte >> 2) & 1;
                    binaryArray[index++] = (byte >> 1) & 1;
                    binaryArray[index++] = byte & 1;
                }
                
                result = binaryArray.join('');
                
            } else if (operation === 'binary-to-mp4') {
                // Convert binary string to bytes
                const dataStr = data.toString();
                const validBits = dataStr.replace(/[^01]/g, ''); // Filter valid bits
                
                const bytes = new Uint8Array(Math.floor(validBits.length / 8));
                
                for (let i = 0; i < bytes.length; i++) {
                    const binaryByte = validBits.substr(i * 8, 8);
                    bytes[i] = parseInt(binaryByte, 2);
                }
                
                result = bytes;
            }
            
            parentPort.postMessage({ 
                chunkIndex, 
                data: result 
            });
            
        } catch (error) {
            parentPort.postMessage({ 
                chunkIndex, 
                error: error.message 
            });
        }
    });
}

// Main execution
async function main() {
    if (!isMainThread) return;
    
    try {
        console.log('=== MP4 Binary Converter Performance Test ===');
        console.log(`Using ${NUM_WORKERS} worker threads`);
        
        const converter = new PerformantMP4Converter();
        
        // Test mp4_to_binary
        console.log('\n1. Converting MP4 to binary...');
        await converter.mkvToBinary('input.mkv', 'output_binary.txt');
        
        // Test binary_to_mp4
        console.log('\n2. Converting binary to MP4...');
        await converter.binaryToMp4('output_binary.txt', 'reconstructed.mkv');
        
        console.log('\n=== Test Complete ===');
        
    } catch (error) {
        console.error('Error:', error.message);
        process.exit(1);
    }
}

if (require.main === module) {
    main();
}

module.exports = { PerformantMP4Converter };
