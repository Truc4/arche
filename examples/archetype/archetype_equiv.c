#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Equivalent C code for test_archetype.arche */

typedef struct {
    double x, y, z;
} Vec3;

/* Player archetype structure */
typedef struct {
    double drag;                  /* meta field */
    Vec3 *pos;                    /* col field - array */
    Vec3 *vel;                    /* col field - array */
    double *health;               /* col field - array */
    int count;
} Player;

/* Enemy archetype structure */
typedef struct {
    double aggression;            /* meta field */
    Vec3 *pos;                    /* col field - array */
    Vec3 *vel;                    /* col field - array */
    int count;
} Enemy;

/* Initialize archetype (allocate memory) */
Player *player_alloc(int capacity) {
    Player *p = malloc(sizeof(Player));
    p->pos = malloc(capacity * sizeof(Vec3));
    p->vel = malloc(capacity * sizeof(Vec3));
    p->health = malloc(capacity * sizeof(double));
    p->count = capacity;
    p->drag = 0.98;  /* default drag */

    /* Initialize some data */
    for (int i = 0; i < capacity; i++) {
        p->pos[i] = (Vec3){0, 0, 0};
        p->vel[i] = (Vec3){1, 1, 1};
        p->health[i] = 100.0;
    }

    return p;
}

Enemy *enemy_alloc(int capacity) {
    Enemy *e = malloc(sizeof(Enemy));
    e->pos = malloc(capacity * sizeof(Vec3));
    e->vel = malloc(capacity * sizeof(Vec3));
    e->count = capacity;
    e->aggression = 0.75;  /* default aggression */

    /* Initialize some data */
    for (int i = 0; i < capacity; i++) {
        e->pos[i] = (Vec3){10, 10, 10};
        e->vel[i] = (Vec3){-0.5, -0.5, -0.5};
    }

    return e;
}

void player_free(Player *p) {
    free(p->pos);
    free(p->vel);
    free(p->health);
    free(p);
}

void enemy_free(Enemy *e) {
    free(e->pos);
    free(e->vel);
    free(e);
}

/* System: move - equivalent to "sys move(pos, vel) { pos = pos + vel; }" */
void move_player(Player *p) {
    for (int i = 0; i < p->count; i++) {
        p->pos[i].x += p->vel[i].x;
        p->pos[i].y += p->vel[i].y;
        p->pos[i].z += p->vel[i].z;
    }
}

void move_enemy(Enemy *e) {
    for (int i = 0; i < e->count; i++) {
        e->pos[i].x += e->vel[i].x;
        e->pos[i].y += e->vel[i].y;
        e->pos[i].z += e->vel[i].z;
    }
}

/* System: dampen - equivalent to "sys dampen(vel, drag) { vel = vel * drag; }" */
void dampen_player(Player *p) {
    for (int i = 0; i < p->count; i++) {
        p->vel[i].x *= p->drag;
        p->vel[i].y *= p->drag;
        p->vel[i].z *= p->drag;
    }
}

void dampen_enemy(Enemy *e) {
    double base_drag = 0.95;
    for (int i = 0; i < e->count; i++) {
        double drag = base_drag * e->aggression;
        e->vel[i].x *= drag;
        e->vel[i].y *= drag;
        e->vel[i].z *= drag;
    }
}

int main() {
    printf("=== C Equivalent: Archetype Example ===\n\n");

    /* Initialize archetypes */
    Player *players = player_alloc(10);
    Enemy *enemies = enemy_alloc(5);

    printf("Initial state:\n");
    printf("Player[0]: pos=(%.1f,%.1f,%.1f) vel=(%.2f,%.2f,%.2f) health=%.1f drag=%.2f\n",
           players->pos[0].x, players->pos[0].y, players->pos[0].z,
           players->vel[0].x, players->vel[0].y, players->vel[0].z,
           players->health[0], players->drag);
    printf("Enemy[0]: pos=(%.1f,%.1f,%.1f) vel=(%.2f,%.2f,%.2f) aggression=%.2f\n",
           enemies->pos[0].x, enemies->pos[0].y, enemies->pos[0].z,
           enemies->vel[0].x, enemies->vel[0].y, enemies->vel[0].z,
           enemies->aggression);

    printf("\nApplying move system...\n");
    move_player(players);
    move_enemy(enemies);

    printf("After move:\n");
    printf("Player[0]: pos=(%.1f,%.1f,%.1f)\n",
           players->pos[0].x, players->pos[0].y, players->pos[0].z);
    printf("Enemy[0]: pos=(%.1f,%.1f,%.1f)\n",
           enemies->pos[0].x, enemies->pos[0].y, enemies->pos[0].z);

    printf("\nApplying dampen system...\n");
    dampen_player(players);
    dampen_enemy(enemies);

    printf("After dampen:\n");
    printf("Player[0]: vel=(%.4f,%.4f,%.4f)\n",
           players->vel[0].x, players->vel[0].y, players->vel[0].z);
    printf("Enemy[0]: vel=(%.4f,%.4f,%.4f)\n",
           enemies->vel[0].x, enemies->vel[0].y, enemies->vel[0].z);

    printf("\nFinal state (50 iterations):\n");
    for (int iter = 0; iter < 50; iter++) {
        move_player(players);
        move_enemy(enemies);
        dampen_player(players);
        dampen_enemy(enemies);
    }

    printf("Player[0]: pos=(%.2f,%.2f,%.2f) vel=%.6f\n",
           players->pos[0].x, players->pos[0].y, players->pos[0].z,
           sqrt(players->vel[0].x * players->vel[0].x +
                players->vel[0].y * players->vel[0].y +
                players->vel[0].z * players->vel[0].z));
    printf("Enemy[0]: pos=(%.2f,%.2f,%.2f) vel=%.6f\n",
           enemies->pos[0].x, enemies->pos[0].y, enemies->pos[0].z,
           sqrt(enemies->vel[0].x * enemies->vel[0].x +
                enemies->vel[0].y * enemies->vel[0].y +
                enemies->vel[0].z * enemies->vel[0].z));

    /* Cleanup */
    player_free(players);
    enemy_free(enemies);

    return 0;
}
