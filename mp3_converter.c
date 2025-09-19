#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

// Thread data structure
typedef struct {
    unsigned char* buffer;
    long start_pos;
    long end_pos;
    FILE* output_file;
    pthread_mutex_t* file_mutex;
    int thread_id;
} thread_data_t;

typedef struct {
    char* binary_data;
    long start_pos;
    long end_pos;
    FILE* output_file;
    pthread_mutex_t* file_mutex;
    int thread_id;
} binary_thread_data_t;

int get_cpu_count() {
    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores > 0 ? cores : 4; // Default to 4 if detection fails
}

void* mp3_to_binary_worker(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    
    // Calculate chunk size for this thread
    long chunk_size = data->end_pos - data->start_pos;
    char* local_buffer = malloc(chunk_size * 8 + 1); // 8 bits per byte + null terminator
    if (!local_buffer) {
        printf("Thread %d: Memory allocation failed\n", data->thread_id);
        return NULL;
    }
    
    long buffer_pos = 0;
    
    // Convert bytes to binary string
    for (long i = data->start_pos; i < data->end_pos; i++) {
        unsigned char byte = data->buffer[i];
        for (int bit = 7; bit >= 0; bit--) {
            local_buffer[buffer_pos++] = (byte & (1 << bit)) ? '1' : '0';
        }
    }
    
    // Write to file (thread-safe)
    pthread_mutex_lock(data->file_mutex);
    fseek(data->output_file, data->start_pos * 8, SEEK_SET);
    fwrite(local_buffer, 1, buffer_pos, data->output_file);
    pthread_mutex_unlock(data->file_mutex);
    
    free(local_buffer);
    return NULL;
}

void* binary_to_mp3_worker(void* arg) {
    binary_thread_data_t* data = (binary_thread_data_t*)arg;
    
    long chunk_bits = data->end_pos - data->start_pos;
    long chunk_bytes = chunk_bits / 8;
    unsigned char* local_buffer = malloc(chunk_bytes);
    
    if (!local_buffer) {
        printf("Thread %d: Memory allocation failed\n", data->thread_id);
        return NULL;
    }
    
    long byte_pos = 0;
    
    // Convert binary string to bytes
    for (long i = data->start_pos; i < data->end_pos; i += 8) {
        unsigned char byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            if (i + bit < data->end_pos && data->binary_data[i + bit] == '1') {
                byte |= (1 << (7 - bit));
            }
        }
        local_buffer[byte_pos++] = byte;
    }
    
    // Write to file (thread-safe)
    pthread_mutex_lock(data->file_mutex);
    fseek(data->output_file, data->start_pos / 8, SEEK_SET);
    fwrite(local_buffer, 1, byte_pos, data->output_file);
    pthread_mutex_unlock(data->file_mutex);
    
    free(local_buffer);
    return NULL;
}

int mp3_to_binary_threaded(const char* input, const char* output) {
    FILE* input_file = fopen(input, "rb");
    if (!input_file) {
        printf("Error: Cannot open input file %s\n", input);
        return 1;
    }

    FILE* output_file = fopen(output, "w");
    if (!output_file) {
        printf("Error: Cannot create output file %s\n", output);
        fclose(input_file);
        return 1;
    }

    // Get file size
    fseek(input_file, 0, SEEK_END);
    long file_size = ftell(input_file);
    fseek(input_file, 0, SEEK_SET);

    // Allocate buffer
    unsigned char* buffer = malloc(file_size);
    if (!buffer) {
        printf("Error: Cannot allocate memory\n");
        fclose(input_file);
        fclose(output_file);
        return 1;
    }

    // Read entire file
    fread(buffer, 1, file_size, input_file);
    fclose(input_file);

    // Get number of threads
    int num_threads = get_cpu_count();
    printf("Using %d threads for MP3 to binary conversion\n", num_threads);

    // Calculate chunk size per thread
    long chunk_size = file_size / num_threads;
    long remainder = file_size % num_threads;

    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    thread_data_t* thread_data = malloc(num_threads * sizeof(thread_data_t));
    pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

    // Create threads
    long current_pos = 0;
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].buffer = buffer;
        thread_data[i].start_pos = current_pos;
        thread_data[i].end_pos = current_pos + chunk_size + (i < remainder ? 1 : 0);
        thread_data[i].output_file = output_file;
        thread_data[i].file_mutex = &file_mutex;
        thread_data[i].thread_id = i;

        pthread_create(&threads[i], NULL, mp3_to_binary_worker, &thread_data[i]);
        current_pos = thread_data[i].end_pos;
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&file_mutex);
    free(threads);
    free(thread_data);
    free(buffer);
    fclose(output_file);
    
    printf("✅ Done! Binary string saved to %s\n", output);
    return 0;
}

int binary_to_mp3_threaded(const char* input, const char* output) {
    FILE* input_file = fopen(input, "r");
    if (!input_file) {
        printf("Error: Cannot open input file %s\n", input);
        return 1;
    }

    // Get file size
    fseek(input_file, 0, SEEK_END);
    long binary_size = ftell(input_file);
    fseek(input_file, 0, SEEK_SET);

    // Allocate buffer for binary data
    char* binary_data = malloc(binary_size + 1);
    if (!binary_data) {
        printf("Error: Cannot allocate memory\n");
        fclose(input_file);
        return 1;
    }

    // Read binary data
    fread(binary_data, 1, binary_size, input_file);
    binary_data[binary_size] = '\0';
    fclose(input_file);

    FILE* output_file = fopen(output, "wb");
    if (!output_file) {
        printf("Error: Cannot create output file %s\n", output);
        free(binary_data);
        return 1;
    }

    // Get number of threads
    int num_threads = get_cpu_count();
    printf("Using %d threads for binary to MP3 conversion\n", num_threads);

    // Calculate chunk size per thread (must be multiple of 8)
    long chunk_size = (binary_size / num_threads / 8) * 8;
    long remainder_bits = binary_size - (chunk_size * num_threads);

    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    binary_thread_data_t* thread_data = malloc(num_threads * sizeof(binary_thread_data_t));
    pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

    // Create threads
    long current_pos = 0;
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].binary_data = binary_data;
        thread_data[i].start_pos = current_pos;
        
        if (i == num_threads - 1) {
            // Last thread handles remainder
            thread_data[i].end_pos = binary_size;
        } else {
            thread_data[i].end_pos = current_pos + chunk_size;
        }
        
        thread_data[i].output_file = output_file;
        thread_data[i].file_mutex = &file_mutex;
        thread_data[i].thread_id = i;

        pthread_create(&threads[i], NULL, binary_to_mp3_worker, &thread_data[i]);
        current_pos = thread_data[i].end_pos;
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&file_mutex);
    free(threads);
    free(thread_data);
    free(binary_data);
    fclose(output_file);
    
    printf("✅ Done! MP3 saved as %s\n", output);
    return 0;
}

int main() {
    clock_t start, end;
    double cpu_time_used;

    printf("=== Multithreaded MP3 Binary Converter Performance Test ===\n");
    printf("CPU cores detected: %d\n", get_cpu_count());

    // Test mp3_to_binary_threaded
    printf("\n1. Converting MP3 to binary (multithreaded)...\n");
    start = clock();
    if (mp3_to_binary_threaded("input.mp3", "output_binary.txt") != 0) {
        return 1;
    }
    end = clock();
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Multithreaded MP3 to binary conversion took: %.4f seconds\n", cpu_time_used);

    // Test binary_to_mp3_threaded
    printf("\n2. Converting binary to MP3 (multithreaded)...\n");
    start = clock();
    if (binary_to_mp3_threaded("output_binary.txt", "reconstructed.mp3") != 0) {
        return 1;
    }
    end = clock();
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Multithreaded binary to MP3 conversion took: %.4f seconds\n", cpu_time_used);

    printf("\n=== Test Complete ===\n");
    return 0;
}
