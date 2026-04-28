/*
 * dc_stats_example.c - Example: Using GLdc + rlgl stats in your game loop
 *
 * Add this to your main.c or a debug overlay.
 * Requires: -DGLDC_ENABLE_STATS and optionally -DRLDC_ENABLE_STATS
 *
 * These are declared in gldc_stats.h / rlgl_dc_batch.h respectively.
 */

#include "raylib.h"
#include <stdio.h>

/* Declare the GLdc stats API (defined in gldc_stats.c) */
typedef struct {
    unsigned int frame_no;
    unsigned int draw_arrays_calls;
    unsigned int draw_elements_calls;
    unsigned int submit_vertices_calls;
    unsigned int fast_path_hits;
    unsigned int fast_path_misses;
    unsigned int headers_emitted;
    unsigned int state_dirty_events;
    unsigned int vertices_transformed;
    unsigned int texture_binds;
    unsigned int immediate_begin_calls;
    unsigned int immediate_end_calls;
    unsigned int immediate_vertices;
} GLdcStats;

extern void glKosPrintStats(void);
extern void glKosResetStats(void);
extern const GLdcStats* glKosGetStats(void);

/* Optional: rlgl-side batcher stats (declared in rlgl_dc_batch.h) */
extern void rlDcPrintStats(void);
extern void rlDcResetStats(void);

/*
 * Call this once per frame after EndDrawing().
 * Prints stats every N frames to serial/stdout.
 */
void DumpPerformanceStats(int everyNFrames)
{
    static int frameCounter = 0;
    if (++frameCounter < everyNFrames) return;
    frameCounter = 0;

    /* Print GLdc-side counters */
    glKosPrintStats();

    /* Print rlgl-side batcher counters */
    rlDcPrintStats();

    /* Reset for next measurement window */
    glKosResetStats();
    rlDcResetStats();
}

/*
 * Example main loop skeleton:
 *
 *  InitWindow(640, 480, "DC Perf Test");
 *  while (!WindowShouldClose()) {
 *      BeginDrawing();
 *          ClearBackground(BLACK);
 *          // ... your game drawing here ...
 *      EndDrawing();
 *      DumpPerformanceStats(60);  // Print every 60 frames
 *  }
 *  CloseWindow();
 */

/* ---------------------------------------------------------------
 * Microbenchmark scenes for isolating specific bottlenecks.
 * Run each one independently and compare counter output.
 * --------------------------------------------------------------- */

/* Scene 1: Same-texture quad storm — isolates draw-call overhead */
void BenchSameTextureQuads(Texture2D tex, int count)
{
    for (int i = 0; i < count; i++) {
        DrawTexture(tex, (i * 17) % 600, (i * 13) % 440, WHITE);
    }
}

/* Scene 2: Alternating-texture quad storm — isolates texture churn */
void BenchAlternatingTextureQuads(Texture2D texA, Texture2D texB, int count)
{
    for (int i = 0; i < count; i++) {
        Texture2D t = (i % 2 == 0) ? texA : texB;
        DrawTexture(t, (i * 17) % 600, (i * 13) % 440, WHITE);
    }
}

/* Scene 3: Text stress — isolates helper-heavy small-draw workload */
void BenchTextStress(Font font, int lines)
{
    for (int i = 0; i < lines; i++) {
        DrawTextEx(font, "Performance benchmark text line.",
                   (Vector2){10, (float)(i * 14)}, 14, 1, WHITE);
    }
}

/* Scene 4: Shape stress — isolates rshapes helper overhead */
void BenchShapeStress(int count)
{
    for (int i = 0; i < count; i++) {
        DrawRectangle((i * 23) % 600, (i * 19) % 440, 16, 16,
                      (Color){(unsigned char)(i*7), 100, 200, 255});
    }
}
