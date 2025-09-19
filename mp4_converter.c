#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

// Thread data structures
typedef struct {
    const char* input_file;
    long start_pos;
    long end_pos;
    FILE* output_file;
    pthread_mutex_t* file_mutex;
    pthread_mutex_t* progress_mutex;
    long* total_processed;
    long total_size;
    int thread_id;
} mp4_thread_data_t;

typedef struct {
    const char* input_file;
    long start_pos;
    long end_pos;
    FILE* output_file;
    pthread_mutex_t* file_mutex;
    pthread_mutex_t* progress_mutex;
    long* total_processed;
    long total_size;
    int thread_id;
} binary_thread_data_t;

int get_cpu_count() {
    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores > 0 ? cores : 10; // Default to 4 if detection fails
}

void update_progress(pthread_mutex_t* progress_mutex, long* total_processed, 
                    long bytes_added, long total_size, const char* operation) {
    pthread_mutex_lock(progress_mutex);
    *total_processed += bytes_added;
    
    // Show progress every MB
    if (*total_processed % (1024 * 1024) < bytes_added) {
        printf("%s Progress: %ld MB / %ld MB (%.1f%%)\n", 
               operation,
               *total_processed / (1024 * 1024),
               total_size / (1024 * 1024),
               (*total_processed * 100.0) / total_size);
    }
    pthread_mutex_unlock(progress_mutex);
}

void* mp4_to_binary_worker(void* arg) {
    mp4_thread_data_t* data = (mp4_thread_data_t*)arg;
    
    FILE* input_file = fopen(data->input_file, "rb");
    if (!input_file) {
        printf("Thread %d: Cannot open input file\n", data->thread_id);
        return NULL;
    }
    
    // Seek to start position
    fseek(input_file, data->start_pos, SEEK_SET);
    
    const int CHUNK_SIZE = 8192; // 8KB chunks per thread
    unsigned char buffer[CHUNK_SIZE];
    char binary_buffer[CHUNK_SIZE * 8 + 1];
    
    long bytes_to_process = data->end_pos - data->start_pos;
    long bytes_processed = 0;
    
    while (bytes_processed < bytes_to_process) {
        int bytes_to_read = (bytes_to_process - bytes_processed > CHUNK_SIZE) ?
                           CHUNK_SIZE : (bytes_to_process - bytes_processed);
        
        int bytes_read = fread(buffer, 1, bytes_to_read, input_file);
        if (bytes_read <= 0) break;
        
        // Convert chunk to binary string
        int binary_pos = 0;
        for (int i = 0; i < bytes_read; i++) {
            unsigned char byte = buffer[i];
            for (int bit = 7; bit >= 0; bit--) {
                binary_buffer[binary_pos++] = (byte & (1 << bit)) ? '1' : '0';
            }
        }
        
        // Write to output file (thread-safe)
        pthread_mutex_lock(data->file_mutex);
        fseek(data->output_file, (data->start_pos + bytes_processed) * 8, SEEK_SET);
        fwrite(binary_buffer, 1, binary_pos, data->output_file);
        pthread_mutex_unlock(data->file_mutex);
        
        bytes_processed += bytes_read;
        update_progress(data->progress_mutex, data->total_processed, 
                       bytes_read, data->total_size, "MP4->Binary");
    }
    
    fclose(input_file);
    return NULL;
}

void* binary_to_mp4_worker(void* arg) {
    binary_thread_data_t* data = (binary_thread_data_t*)arg;
    
    FILE* input_file = fopen(data->input_file, "r");
    if (!input_file) {
        printf("Thread %d: Cannot open input file\n", data->thread_id);
        return NULL;
    }
    
    // Seek to start position
    fseek(input_file, data->start_pos, SEEK_SET);
    
    long bits_to_process = data->end_pos - data->start_pos;
    char bit_buffer[8];
    unsigned char byte_buffer[1024];
    int bit_count = 0;
    int byte_count = 0;
    int c;
    long bits_processed = 0;
    
    while (bits_processed < bits_to_process && (c = fgetc(input_file)) != EOF) {
        if (c == '0' || c == '1') {
            bit_buffer[bit_count++] = c;
            bits_processed++;
            
            if (bit_count == 8) {
                // Convert 8-bit string to byte
                unsigned char byte = 0;
                for (int i = 0; i < 8; i++) {
                    if (bit_buffer[i] == '1') {
                        byte |= (1 << (7 - i));
                    }
                }
                byte_buffer[byte_count++] = byte;
                bit_count = 0;
                
                // Flush buffer when full
                if (byte_count >= 1024) {
                    pthread_mutex_lock(data->file_mutex);
                    fseek(data->output_file, (data->start_pos / 8) + (bits_processed / 8) - byte_count, SEEK_SET);
                    fwrite(byte_buffer, 1, byte_count, data->output_file);
                    pthread_mutex_unlock(data->file_mutex);
                    
                    update_progress(data->progress_mutex, data->total_processed, 
                                   byte_count, data->total_size / 8, "Binary->MP4");
                    byte_count = 0;
                }
            }
        }
    }
    
    // Flush remaining bytes
    if (byte_count > 0) {
        pthread_mutex_lock(data->file_mutex);
        fseek(data->output_file, (data->start_pos / 8) + (bits_processed / 8) - byte_count, SEEK_SET);
        fwrite(byte_buffer, 1, byte_count, data->output_file);
        pthread_mutex_unlock(data->file_mutex);
        
        update_progress(data->progress_mutex, data->total_processed, 
                       byte_count, data->total_size / 8, "Binary->MP4");
    }
    
    fclose(input_file);
    return NULL;
}

int mp4_to_binary_threaded(const char* input, const char* output) {
    FILE* input_file = fopen(input, "rb");
    if (!input_file) {
        printf("Error: Cannot open input file %s\n", input);
        return 1;
    }

    // Get file size
    fseek(input_file, 0, SEEK_END);
    long file_size = ftell(input_file);
    fclose(input_file);

    printf("Processing MP4 file: %ld bytes\n", file_size);

    FILE* output_file = fopen(output, "w");
    if (!output_file) {
        printf("Error: Cannot create output file %s\n", output);
        return 1;
    }

    // Get number of threads
    int num_threads = get_cpu_count();
    printf("Using %d threads for MP4 to binary conversion\n", num_threads);

    // Calculate chunk size per thread
    long chunk_size = file_size / num_threads;
    long remainder = file_size % num_threads;

    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    mp4_thread_data_t* thread_data = malloc(num_threads * sizeof(mp4_thread_data_t));
    pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
    long total_processed = 0;

    // Create threads
    long current_pos = 0;
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].input_file = input;
        thread_data[i].start_pos = current_pos;
        thread_data[i].end_pos = current_pos + chunk_size + (i < remainder ? 1 : 0);
        thread_data[i].output_file = output_file;
        thread_data[i].file_mutex = &file_mutex;
        thread_data[i].progress_mutex = &progress_mutex;
        thread_data[i].total_processed = &total_processed;
        thread_data[i].total_size = file_size;
        thread_data[i].thread_id = i;

        pthread_create(&threads[i], NULL, mp4_to_binary_worker, &thread_data[i]);
        current_pos = thread_data[i].end_pos;
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&progress_mutex);
    free(threads);
    free(thread_data);
    fclose(output_file);
    
    printf("✅ Done! Binary string saved to %s\n", output);
    return 0;
}

int binary_to_mp4_threaded(const char* input, const char* output) {
    FILE* input_file = fopen(input, "r");
    if (!input_file) {
        printf("Error: Cannot open input file %s\n", input);
        return 1;
    }

    // Get binary file size
    fseek(input_file, 0, SEEK_END);
    long binary_file_size = ftell(input_file);
    fclose(input_file);

    printf("Processing binary file: %ld bytes\n", binary_file_size);

    FILE* output_file = fopen(output, "wb");
    if (!output_file) {
        printf("Error: Cannot create output file %s\n", output);
        return 1;
    }

    // Get number of threads
    int num_threads = get_cpu_count();
    printf("Using %d threads for binary to MP4 conversion\n", num_threads);

    // Calculate chunk size per thread (must be multiple of 8 for byte alignment)
    long chunk_size = (binary_file_size / num_threads / 8) * 8;
    long remainder_bits = binary_file_size - (chunk_size * num_threads);

    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    binary_thread_data_t* thread_data = malloc(num_threads * sizeof(binary_thread_data_t));
    pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
    long total_processed = 0;

    // Create threads
    long current_pos = 0;
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].input_file = input;
        thread_data[i].start_pos = current_pos;
        
        if (i == num_threads - 1) {
            // Last thread handles remainder
            thread_data[i].end_pos = binary_file_size;
        } else {
            thread_data[i].end_pos = current_pos + chunk_size;
        }
        
        thread_data[i].output_file = output_file;
        thread_data[i].file_mutex = &file_mutex;
        thread_data[i].progress_mutex = &progress_mutex;
        thread_data[i].total_processed = &total_processed;
        thread_data[i].total_size = binary_file_size;
        thread_data[i].thread_id = i;

        pthread_create(&threads[i], NULL, binary_to_mp4_worker, &thread_data[i]);
        current_pos = thread_data[i].end_pos;
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&progress_mutex);
    free(threads);
    free(thread_data);
    fclose(output_file);
    
    printf("✅ Done! MP4 saved as %s\n", output);
    return 0;
}

int main() {
    clock_t start, end;
    double cpu_time_used;

    printf("=== Multithreaded MP4 Binary Converter Performance Test ===\n");
    printf("CPU cores detected: %d\n", get_cpu_count());

    // Test mp4_to_binary_threaded
    printf("\n1. Converting MP4 to binary (multithreaded)...\n");
    start = clock();
    if (mp4_to_binary_threaded("input.mkv", "output_binary.txt") != 0) {
        return 1;
    }
    end = clock();
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Multithreaded MP4 to binary conversion took: %.4f seconds\n", cpu_time_used);

    // Test binary_to_mp4_threaded
    printf("\n2. Converting binary to MP4 (multithreaded)...\n");
    start = clock();
    if (binary_to_mp4_threaded("output_binary.txt", "reconstructed.mkv") != 0) {
        return 1;
    }
    end = clock();
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Multithreaded binary to MP4 conversion took: %.4f seconds\n", cpu_time_used);

    printf("\n=== Test Complete ===\n");
    return 0;
}
