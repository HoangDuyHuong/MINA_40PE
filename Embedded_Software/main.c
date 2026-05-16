// gcc fpga_ecg_console.c -o fpga_ecg_console -O2 -lm
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#define BILLION  1000000000

// Write channel
#define START_BASE              (0x00000)
#define LDM_INPUT_BASE_PHYS     (0x10000>>2)
#define CRAM_INPUT_BASE_PHYS    (0x20000>>2)
#define WRAM_INPUT_BASE_PHYS    (0x30000>>2)
#define BRAM_INPUT_BASE_PHYS    (0x40000>>2)

// Read channel
#define DONE_BASE_PHYS          (0x00000)
#define LDM_OUTPUT_BASE_PHYS    (0x10000>>2)

#define FRACTIONAL_BITS 6
#define SCALE_FACTOR (1 << FRACTIONAL_BITS)

#define NumberOfPicture 100
#define d 1
#define SEG_LEN 320

#include "CGRA.h"
#include "FPGA_Driver.c"

// Vietnamese class names
static const char* VN_LABELS[5] = {
    "Bình thường",                 // 0 = N
    "Ngoại tâm thu trên thất",     // 1 = S
    "Ngoại tâm thu thất",          // 2 = V
    "Nhịp hợp nhất",               // 3 = F
    "Không xác định"               // 4 = Q/Other
};

// English class names
static const char* EN_LABELS[5] = {
    "Normal",                      // 0 = N
    "Supraventricular",           // 1 = S
    "Ventricular",                // 2 = V
    "Fusion",                     // 3 = F
    "Unknown"                     // 4 = Q/Other
};

float fixed_point_to_float(U32 fx){
    // Ép kiểu trực tiếp sang số 16-bit có dấu chuẩn của C để xử lý số bù 2
    int16_t s_fx = (int16_t)(fx & 0xFFFF); 
    return (float)s_fx / SCALE_FACTOR;
}

U32 FX_convert(float x){
    float s = x * SCALE_FACTOR;
    int16_t f = (s >= 0) ? (int16_t)(s + 0.5f) : (int16_t)(s - 0.5f);
    if (f > 32767) f = 32767;
    else if (f < -32768) f = -32768;
    return (U32)(f & 0xFFFF);
}
float fixedpoint_converter(float value) {
	if(value >= 512){
		value = value - 512;	
	}
    float scalingFactor = 64.0f; // 2^5
    return round(value * scalingFactor) / scalingFactor; 
}
void print_progress(int current, int total, int bar_width) {
    float progress = (float)current / total;
    int pos = bar_width * progress;
    
    printf("\r[");
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %d/%d (%.1f%%)", current, total, progress * 100.0);
    fflush(stdout);
}

static void softmax5(const float in[5], float out[5]) {
    float maxv = in[0];
    for (int i = 1; i < 5; i++) if (in[i] > maxv) maxv = in[i];

    float sum = 0.0f;
    for (int i = 0; i < 5; i++) {
        out[i] = expf(in[i] - maxv);
        sum += out[i];
    }
    if (sum < 1e-20f) sum = 1e-20f;
    for (int i = 0; i < 5; i++) out[i] /= sum;
}

// ===== NEW: Print detailed computation values =====
void print_detailed_output(int sample_idx, 
                          float* cnn_out, 
                          float* gap_out, 
                          float* dense_logits,
                          float* softmax_probs,
                          int gt, int pred,
                          FILE* detail_file)
{
    printf("\n");
    printf("========================================\n");
    printf("Sample #%d - Ground Truth: %d (%s)\n", 
           sample_idx + 1, gt, (gt>=0 && gt<5) ? EN_LABELS[gt] : "Unknown");
    printf("========================================\n");
    
    // 1. CNN Output statistics (1280 values)
    float cnn_min = cnn_out[0], cnn_max = cnn_out[0], cnn_mean = 0.0f;
    for(int i = 0; i < 1280; i++) {
        if(cnn_out[i] < cnn_min) cnn_min = cnn_out[i];
        if(cnn_out[i] > cnn_max) cnn_max = cnn_out[i];
        cnn_mean += cnn_out[i];
    }
    cnn_mean /= 1280.0f;
    
    printf("\n[1] CNN Output (1280 values):\n");
    printf("    Range: [%.4f, %.4f]\n", cnn_min, cnn_max);
    printf("    Mean:  %.4f\n", cnn_mean);
    printf("    First 10: ");
    for(int i = 0; i < 1280; i++) printf("%.4f ", cnn_out[i]);
    printf("\n");
    
    // 2. Global Average Pooling Output (32 values)
    printf("\n[2] Global Average Pooling (32 channels):\n");
    printf("    ");
    for(int i = 0; i < 32; i++) {
        printf("%.4f ", gap_out[i]);
        if((i+1) % 8 == 0) printf("\n    ");
    }
    printf("\n");
    
    // 3. Dense Layer Logits (before activation)
    printf("\n[3] Dense Layer Logits (5 classes):\n");
    printf("    Class 0 (N): %.4f\n", dense_logits[0]);
    printf("    Class 1 (S): %.4f\n", dense_logits[1]);
    printf("    Class 2 (V): %.4f\n", dense_logits[2]);
    printf("    Class 3 (F): %.4f\n", dense_logits[3]);
    printf("    Class 4 (Q): %.4f\n", dense_logits[4]);
    
    // 4. Softmax Probabilities
    printf("\n[4] Softmax Probabilities:\n");
    for(int i = 0; i < 5; i++) {
        printf("    Class %d (%s): %.6f (%.2f%%)\n", 
               i, EN_LABELS[i], softmax_probs[i], softmax_probs[i] * 100.0f);
    }
    
    // 5. Prediction
    printf("\n[5] Prediction:\n");
    printf("    Predicted: %d (%s)\n", pred, EN_LABELS[pred]);
    printf("    Confidence: %.2f%%\n", softmax_probs[pred] * 100.0f);
    printf("    Result: %s\n", (gt == pred) ? "✓ CORRECT" : "✗ WRONG");
    
    printf("========================================\n\n");
    
    // Write to file
    if(detail_file) {
        fprintf(detail_file, "\nSample,%d\n", sample_idx + 1);
        fprintf(detail_file, "GroundTruth,%d,%s\n", gt, (gt>=0 && gt<5) ? EN_LABELS[gt] : "Unknown");
        fprintf(detail_file, "Prediction,%d,%s\n", pred, EN_LABELS[pred]);
        
        fprintf(detail_file, "\nCNN_Stats,Min,Max,Mean\n");
        fprintf(detail_file, "Values,%.6f,%.6f,%.6f\n", cnn_min, cnn_max, cnn_mean);
        
        fprintf(detail_file, "\nGAP_Output");
        for(int i = 0; i < 32; i++) fprintf(detail_file, ",%.6f", gap_out[i]);
        fprintf(detail_file, "\n");
        
        fprintf(detail_file, "\nDense_Logits");
        for(int i = 0; i < 5; i++) fprintf(detail_file, ",%.6f", dense_logits[i]);
        fprintf(detail_file, "\n");
        
        fprintf(detail_file, "\nSoftmax_Probs");
        for(int i = 0; i < 5; i++) fprintf(detail_file, ",%.6f", softmax_probs[i]);
        fprintf(detail_file, "\n");
        
        fprintf(detail_file, "Confidence,%.6f\n", softmax_probs[pred]);
        fprintf(detail_file, "Correct,%s\n", (gt == pred) ? "Yes" : "No");
        fprintf(detail_file, "---\n");
    }
}

int main(int argc, char** argv){
    // CLI paths (optional)
    const char *signals_path = (argc >= 2) ? argv[1] : "signals.txt";
    const char *labels_path  = (argc >= 3) ? argv[2] : "labels.txt";

    printf("=================================================\n");
    printf("   FPGA ECG Classification System (Console Mode)\n");
    printf("=================================================\n\n");

    // ===== FPGA init =====
    printf("[1/5] Initializing FPGA...\n");
    unsigned char* membase;
    if (fpga_open() == 0) {
        fprintf(stderr, "ERROR: Cannot open CGRA device!\n");
        exit(1);
    }
    
    
    printf("      ✓ FPGA initialized successfully\n\n");

    // ===== Load configuration files =====
    printf("[2/5] Loading FPGA configuration files...\n");
    // FILE *CRAM_file = fopen("CRAM_File.txt", "r");
    // if(!CRAM_file){ perror("      ERROR: CRAM_File.txt"); return 1; }
    // FILE *WRAM_file = fopen("WRAM_File.txt", "r");
    // if(!WRAM_file){ perror("      ERROR: WRAM_File.txt"); return 1; }
    // FILE *BRAM_file = fopen("BRAM_File.txt", "r");
    // if(!BRAM_file){ perror("      ERROR: BRAM_File.txt"); return 1; }
    // FILE *WRAM_2_file = fopen("WRAM_2_File.txt", "r");
    // if(!WRAM_2_file){ perror("      ERROR: WRAM_2_File.txt"); return 1; }
    // FILE *BRAM_2_file = fopen("BRAM_2_File.txt", "r");
    // if(!BRAM_2_file){ perror("      ERROR: BRAM_2_File.txt"); return 1; }


    FILE *CRAM_file = fopen("CRAM_File.txt", "r");
    if(!CRAM_file){ perror("      ERROR: CRAM_File.txt"); return 1; }
    FILE *WRAM_file = fopen("WRAM_File.txt", "r");
    if(!WRAM_file){ perror("      ERROR: WRAM_File.txt"); return 1; }
    FILE *BRAM_file = fopen("BRAM_File.txt", "r");
    if(!BRAM_file){ perror("      ERROR: BRAM_File.txt"); return 1; }
    FILE *WRAM_2_file = fopen("WRAM_2_File.txt", "r");
    if(!WRAM_2_file){ perror("      ERROR: WRAM_2_File.txt"); return 1; }
    FILE *BRAM_2_file = fopen("BRAM_2_File.txt", "r");
    if(!BRAM_2_file){ perror("      ERROR: BRAM_2_File.txt"); return 1; }
    int i = 0;
    U32 value; 
    float value_f;
    float weight_final[160], bias_final[5];
    // U32 CRAM[42], WRAM[5144], BRAM[244];
    U32 CRAM[42], WRAM[6096], BRAM[196];
    while (fscanf(CRAM_file, "%8x", &value) == 1) CRAM[i++] = value; 
    fclose(CRAM_file);
    printf("      ✓ CRAM loaded: %d values\n", i);
    
    i=0; 
    while (fscanf(WRAM_file, "%4x", &value) == 1) WRAM[i++] = value; 
    fclose(WRAM_file);
    printf("      ✓ WRAM loaded: %d values\n", i);
    
    i=0; 
    while (fscanf(BRAM_file, "%4x", &value) == 1) BRAM[i++] = value; 
    fclose(BRAM_file);
    printf("      ✓ BRAM loaded: %d values\n", i);

    // Write to FPGA registers
    for(int j=0;j<42;j++)   
        *(MY_IP_info.reg_mmap + CRAM_INPUT_BASE_PHYS + j) = CRAM[j];

    // Nạp WRAM (Trọng số Conv) - TUYẾN TÍNH
    for(int j=0; j<6096; j++) {
        *(MY_IP_info.reg_mmap + WRAM_INPUT_BASE_PHYS + j) = WRAM[j];
    }

    // Nạp BRAM (Bias Conv) - TUYẾN TÍNH
    for(int j=0; j<196; j++) {
        *(MY_IP_info.reg_mmap + BRAM_INPUT_BASE_PHYS + j) = BRAM[j];
    }

    i=0; 
    while (fscanf(WRAM_2_file, "%f", &value_f) == 1) weight_final[i++] = value_f; 
    fclose(WRAM_2_file);
    printf("      ✓ Final weights loaded: %d values\n", i);
    
    i=0; 
    while (fscanf(BRAM_2_file, "%f", &value_f) == 1) bias_final[i++] = value_f; 
    fclose(BRAM_2_file);
    printf("      ✓ Final biases loaded: %d values\n\n", i);

    // ===== Load dataset =====
    printf("[3/5] Loading dataset...\n");
    float* InModel = (float*)malloc(NumberOfPicture * d * SEG_LEN * sizeof(float));
    float tmp;
    FILE* Input = fopen(signals_path, "r");
    if(!Input){ 
        perror("      ERROR: signals file"); 
        return 1; 
    }
    for(int k=0;k<NumberOfPicture*d*SEG_LEN;k++){
        if (fscanf(Input, "%f", &tmp)!=1){ 
            fprintf(stderr,"      ERROR: Not enough data in signals file\n"); 
            return 1; 
        }
        InModel[k]=tmp;
    }
    fclose(Input);
    printf("      ✓ Signals loaded: %d samples (%d beats × %d points)\n", 
           NumberOfPicture*SEG_LEN, NumberOfPicture, SEG_LEN);

    float* Label = (float*)malloc(NumberOfPicture * sizeof(float));
    FILE* Output = fopen(labels_path, "r");
    if(!Output){ 
        perror("      ERROR: labels file"); 
        free(InModel); 
        return 1; 
    }
    for(int k=0;k<NumberOfPicture;k++){
        if (fscanf(Output, "%f", &tmp)!=1){ 
            fprintf(stderr,"      ERROR: Not enough labels\n"); 
            return 1; 
        }
        Label[k]=tmp;
    }
    fclose(Output);
    printf("      ✓ Labels loaded: %d labels\n\n", NumberOfPicture);

    // ===== Open detailed output file =====
    FILE *detail_file = fopen("detailed_outputs.csv", "w");
    if(detail_file) {
        fprintf(detail_file, "# Detailed computation outputs for each ECG beat\n");
        fprintf(detail_file, "# Format: Sample info, CNN stats, GAP output, Dense logits, Softmax probs\n");
    }

    // ===== Prepare inference =====
    float* OutArray = (float*)malloc(NumberOfPicture * sizeof(float));
    float CNN_output[1280];
    float GlobalAveragePool1D[32];
    float out_Dense[5];
    float softmax_out[5];
    float Image[d*SEG_LEN];
    U32 Pixel[340];
    uint16_t address[1280];
    struct timespec start_CNN, end_CNN, start_total, end_total;
    unsigned long long time_spent_CNN=0;

    for(int j=0;j<1280;j++){ 
        int a=j/40; 
        address[j]=(j+a*24)&0xFFFF; 
    }

    // ===== Run inference =====
    printf("[4/5] Running inference on FPGA...\n");
    int correct = 0;
    int confusion[5][5] = {0};
    
    clock_gettime(CLOCK_REALTIME, &start_total);

    for(int iimg=0; iimg<NumberOfPicture; iimg++){
        // Load image
        int startIndex = iimg*d*SEG_LEN;
        for(int k=0;k<d*SEG_LEN;k++)
            Image[k]=InModel[startIndex+k];
       
        // Convert to fixed point
        for(int k=0;k<340;k++){
            Pixel[k] = (k<SEG_LEN) ? FX_convert(Image[k]) : FX_convert(0.0f);
        }
        // Timing start
        struct timespec t0, t1;
        clock_gettime(CLOCK_REALTIME, &t0);
     
        // Write input to LDM - Mapping 8 samples per PE
        for(int k=0; k<320; k++) {
            int pe_idx = k / 8;     // Xác định PE nào
            int local_addr = k % 8; // Địa chỉ bên trong PE đó
            int hw_addr = (pe_idx << 6) | local_addr; // Bước nhảy 64 giữa các PE
            *(MY_IP_info.reg_mmap + LDM_INPUT_BASE_PHYS + hw_addr) = Pixel[k];
        }   

        // Start
        *(MY_IP_info.reg_mmap + START_BASE) = 1;

        // Wait done
        while(*(MY_IP_info.reg_mmap + DONE_BASE_PHYS) != 1) {
            usleep(50);
        }
        
        // Read output
        for(int j=0; j<1280; j++) {
            int pe_idx = j / 32;     // 40 PE, mỗi PE có 32 kết quả
            int local_addr = j % 32; 
            int hw_addr = (pe_idx << 6) | local_addr;
            int16_t raw_16bit = *(MY_IP_info.reg_mmap + LDM_OUTPUT_BASE_PHYS + hw_addr);
            CNN_output[j] = fixed_point_to_float(raw_16bit);
        }

        clock_gettime(CLOCK_REALTIME, &t1);
        unsigned long long sample_ns =
            BILLION*(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec);
        time_spent_CNN += sample_ns;

        // Global Average Pooling
        for(int j=0;j<32;j++){
            float avg=0;
            for(int k=0;k<40;k++)
                avg += CNN_output[40*j+k];
            GlobalAveragePool1D[j]=avg/40.0f;
        }

        // Dense layer (logits)
        for(int j=0;j<5;j++){
            float s=0;
            for(int k=0;k<32;k++)
                s += GlobalAveragePool1D[k]*weight_final[k*5 + j];
            out_Dense[j]=s+bias_final[j];
        }
        
        // Softmax probabilities (for display only)
        // softmax5(out_Dense, softmax_out);

        // Prediction (using logits directly - argmax is same)
        int pred=0;
        float best=out_Dense[0];
        for(int j=1;j<5;j++){
            if(out_Dense[j]>best){
                best=out_Dense[j];
                pred=j;
            }
        }
        OutArray[iimg]=(float)pred;

        // Update stats
        int gt=(int)Label[iimg];
        if (gt==pred) correct++;
        if (gt >= 0 && gt < 5 && pred >= 0 && pred < 5)
            confusion[gt][pred]++;

        // ===== Print detailed output for each sample (or first 5 samples) =====
        // Change condition to print for all: if(1)
        // Or print first 5: if(iimg < 5)
        // Or print wrong predictions: if(gt != pred)
        if(iimg < 5) {  // Print first 5 samples in detail
            print_detailed_output(iimg, CNN_output, GlobalAveragePool1D, 
                                out_Dense, softmax_out, gt, pred, detail_file);
        }

        // Progress bar
        if ((iimg+1) % 10 == 0 || iimg == NumberOfPicture-1)
            print_progress(iimg+1, NumberOfPicture, 40);
    }

    clock_gettime(CLOCK_REALTIME, &end_total);
    unsigned long long time_total =
        BILLION*(end_total.tv_sec - start_total.tv_sec) +
        (end_total.tv_nsec - start_total.tv_nsec);

    printf("\n      ✓ Inference completed\n\n");
    
    // Close detail file
    if(detail_file) {
        fclose(detail_file);
        printf("✓ Detailed outputs saved to: detailed_outputs.csv\n\n");
    }
    
    // ===== Display results =====
    printf("[5/5] Results:\n");
    printf("=================================================\n");
    printf("  Overall Accuracy: %.2f%% (%d/%d correct)\n", 
           100.0 * (double)correct / (double)NumberOfPicture,
           correct, NumberOfPicture);
    printf("=================================================\n\n");

    // Timing statistics
    printf("Timing Statistics:\n");
    printf("-------------------------------------------------\n");
    printf("  Total execution time:    %.3f s\n", (double)time_total/BILLION);
    printf("  CNN inference time:      %.3f s\n", (double)time_spent_CNN/BILLION);
    printf("  Average time per sample: %.3f ms\n", 
           (double)time_spent_CNN/BILLION/NumberOfPicture*1000.0);
    printf("  Throughput:              %.1f samples/sec\n", 
           NumberOfPicture/((double)time_spent_CNN/BILLION));
    printf("-------------------------------------------------\n\n");

    // Per-class statistics
    printf("Per-Class Statistics:\n");
    printf("-------------------------------------------------\n");
    printf("Class | Name                  | Accuracy  | Count\n");
    printf("-------------------------------------------------\n");
    
    int class_total[5] = {0};
    int class_correct[5] = {0};
    
    for(int i=0; i<NumberOfPicture; i++){
        int gt = (int)Label[i];
        int pred = (int)OutArray[i];
        if (gt >= 0 && gt < 5) {
            class_total[gt]++;
            if (gt == pred) class_correct[gt]++;
        }
    }
    
    for(int i=0; i<5; i++){
        if (class_total[i] > 0) {
            double acc = 100.0 * class_correct[i] / class_total[i];
            printf("  %d   | %-21s | %5.1f%%   | %d/%d\n", 
                   i, VN_LABELS[i], acc, class_correct[i], class_total[i]);
        } else {
            printf("  %d   | %-21s | N/A       | 0/0\n", i, VN_LABELS[i]);
        }
    }
    printf("-------------------------------------------------\n\n");

    // Confusion Matrix
    printf("Confusion Matrix:\n");
    printf("-------------------------------------------------\n");
    printf("     |");
    for(int i=0; i<5; i++) printf(" %3d |", i);
    printf("\n");
    printf("-----|");
    for(int i=0; i<5; i++) printf("-----|");
    printf("\n");
    
    for(int i=0; i<5; i++){
        printf(" %3d |", i);
        for(int j=0; j<5; j++){
            if (confusion[i][j] > 0)
                printf(" %3d |", confusion[i][j]);
            else
                printf("   . |");
        }
        printf("  %s\n", EN_LABELS[i]);
    }
    printf("-------------------------------------------------\n");
    printf("(Rows: Ground Truth, Columns: Predicted)\n\n");

    // Save results to file
    FILE *results_file = fopen("classification_results.txt", "w");
    if (results_file) {
        fprintf(results_file, "Sample,GroundTruth,Prediction,GroundTruth_Label,Prediction_Label,Correct\n");
        for(int i=0; i<NumberOfPicture; i++){
            int gt = (int)Label[i];
            int pred = (int)OutArray[i];
            fprintf(results_file, "%d,%d,%d,%s,%s,%s\n", 
                    i+1, gt, pred, 
                    (gt>=0 && gt<5) ? EN_LABELS[gt] : "Unknown",
                    (pred>=0 && pred<5) ? EN_LABELS[pred] : "Unknown",
                    (gt==pred) ? "Yes" : "No");
        }
        fclose(results_file);
        printf("✓ Detailed results saved to: classification_results.txt\n\n");
    }

    // Cleanup
    free(InModel); 
    free(Label); 
    free(OutArray);

    printf("=================================================\n");
    printf("           Classification Complete!\n");
    printf("=================================================\n");

    return 0;
}