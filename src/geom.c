/* The wireframe shells. Each is built from the icosahedron/dodecahedron coordinates and scaled so
 * its vertices sit on the same sphere (radius CIRCUM). */
#include "vigil.h"
#include <math.h>
#include <string.h>

#define PHI 1.6180339887f

static float dist(const float *a, const float *b) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

static void norm(float *v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) / CIRCUM;
    v[0] /= l; v[1] /= l; v[2] /= l;
}

/* joins every pair of vertices at the shortest distance found (all edges of these solids are equal) */
static int near_edges(float v[][3], int nv, int e[][2], int max) {
    float m = 1e9f;
    int ne = 0;
    for (int i = 0; i < nv; i++) for (int j = i + 1; j < nv; j++) { float d = dist(v[i], v[j]); if (d < m) m = d; }
    for (int i = 0; i < nv; i++) for (int j = i + 1; j < nv; j++)
        if (dist(v[i], v[j]) < m * 1.05f && ne < max) { e[ne][0] = i; e[ne][1] = j; ne++; }
    return ne;
}

static int ico(float v[][3]) {
    int n = 0;
    for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2) {
        float A = (float)a, B = b * PHI;
        v[n][0] = 0; v[n][1] = A; v[n][2] = B; n++;
        v[n][0] = A; v[n][1] = B; v[n][2] = 0; n++;
        v[n][0] = B; v[n][1] = 0; v[n][2] = A; n++;
    }
    return n;
}

static void dodeca(Shape *s) {
    int n = 0;
    for (int x = -1; x <= 1; x += 2) for (int y = -1; y <= 1; y += 2) for (int z = -1; z <= 1; z += 2) {
        s->v[n][0] = (float)x; s->v[n][1] = (float)y; s->v[n][2] = (float)z; n++;
    }
    for (int a = -1; a <= 1; a += 2) for (int b = -1; b <= 1; b += 2) {
        float A = a / PHI, B = b * PHI;
        s->v[n][0] = 0; s->v[n][1] = A; s->v[n][2] = B; n++;
        s->v[n][0] = A; s->v[n][1] = B; s->v[n][2] = 0; n++;
        s->v[n][0] = B; s->v[n][1] = 0; s->v[n][2] = A; n++;
    }
    s->nv = n;
    s->ne = near_edges(s->v, n, s->e, MAX_E);
}

/* truncated icosahedron: two points on each icosahedron edge */
static void bucky(Shape *s) {
    float iv[12][3];
    int ie[30][2], n = 0;
    ico(iv);
    int nie = near_edges(iv, 12, ie, 30);
    for (int k = 0; k < nie; k++) for (int t = 1; t <= 2; t++) {
        for (int q = 0; q < 3; q++) s->v[n][q] = iv[ie[k][0]][q] + (iv[ie[k][1]][q] - iv[ie[k][0]][q]) * t / 3.0f;
        norm(s->v[n]); n++;
    }
    s->nv = n;
    s->ne = near_edges(s->v, n, s->e, MAX_E);
}

static int add_vertex(Shape *s, const float *p) {
    float v[3] = {p[0], p[1], p[2]};
    norm(v);
    for (int i = 0; i < s->nv; i++) if (dist(s->v[i], v) < 1e-3f) return i;
    if (s->nv >= MAX_V) return 0;
    memcpy(s->v[s->nv], v, sizeof v);
    return s->nv++;
}

static void add_edge(Shape *s, int a, int b) {
    if (a == b) return;
    for (int i = 0; i < s->ne; i++)
        if ((s->e[i][0] == a && s->e[i][1] == b) || (s->e[i][0] == b && s->e[i][1] == a)) return;
    if (s->ne >= MAX_E) return;
    s->e[s->ne][0] = a; s->e[s->ne][1] = b; s->ne++;
}

static void add_tri(Shape *s, int a, int b, int c) {
    if (s->nt >= MAX_T) return;
    s->t[s->nt][0] = a; s->t[s->nt][1] = b; s->t[s->nt][2] = c; s->nt++;
}

static int edge_of(const Shape *s, int a, int b) {
    for (int i = 0; i < s->ne; i++) if ((s->e[i][0] == a && s->e[i][1] == b) || (s->e[i][0] == b && s->e[i][1] == a)) return i;
    return -1;
}

/* geodesic sphere: each icosahedron face split into n*n triangles and pushed out to the sphere */
static void geo(Shape *s, int n) {
    float iv[12][3];
    int ie[30][2], g[8][8];
    ico(iv);
    int nie = near_edges(iv, 12, ie, 30);
    char has[12][12];
    memset(has, 0, sizeof has);
    for (int k = 0; k < nie; k++) has[ie[k][0]][ie[k][1]] = has[ie[k][1]][ie[k][0]] = 1;
    s->nv = s->ne = 0;
    for (int i = 0; i < 12; i++) for (int j = i + 1; j < 12; j++) for (int k = j + 1; k < 12; k++) {
        if (!(has[i][j] && has[j][k] && has[i][k])) continue;
        for (int a = 0; a <= n; a++) for (int b = 0; b <= n - a; b++) {
            float p[3];
            for (int q = 0; q < 3; q++) p[q] = (iv[i][q] * a + iv[j][q] * b + iv[k][q] * (n - a - b)) / n;
            g[a][b] = add_vertex(s, p);
        }
        for (int a = 0; a < n; a++) for (int b = 0; b < n - a; b++) {
            add_edge(s, g[a][b], g[a + 1][b]);
            add_edge(s, g[a][b], g[a][b + 1]);
            add_edge(s, g[a + 1][b], g[a][b + 1]);
            add_tri(s, g[a][b], g[a + 1][b], g[a][b + 1]);                      /* pointing one way */
            if (b + 1 <= n - a - 1) add_tri(s, g[a + 1][b], g[a + 1][b + 1], g[a][b + 1]); /* and the other */
        }
    }
}

int shape_make(Shape *s, const char *name) {
    memset(s, 0, sizeof *s);
    if (!strcmp(name, "dodeca")) dodeca(s);
    else if (!strcmp(name, "bucky")) bucky(s);
    else if (!strcmp(name, "geo2")) geo(s, 2);
    else if (!strcmp(name, "geo3")) geo(s, 3);
    else return 0;
    for (int i = 0; i < s->ne; i++) for (int k = 0; k < 2; k++) {
        int v = s->e[i][k];
        if (s->nadj[v] < MAX_DEG) { s->vn[v][s->nadj[v]] = s->e[i][1 - k]; s->adj[v][s->nadj[v]++] = i; }
    }
    /* triangle normals, which triangles share each edge, and the triangles around each vertex */
    for (int i = 0; i < s->ne; i++) s->et[i][0] = s->et[i][1] = -1;
    for (int t = 0; t < s->nt; t++) {
        float n[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) for (int q = 0; q < 3; q++) n[q] += s->v[s->t[t][k]][q];
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int q = 0; q < 3; q++) s->tn[t][q] = n[q] / l;
        for (int k = 0; k < 3; k++) {
            int e = edge_of(s, s->t[t][k], s->t[t][(k + 1) % 3]);
            if (e >= 0) s->et[e][s->et[e][0] < 0 ? 0 : 1] = t;
            int v = s->t[t][k];
            if (s->nvt[v] < MAX_DEG) s->vt[v][s->nvt[v]++] = t;
        }
    }
    for (int t = 0; t < s->nt; t++) for (int k = 0; k < 3; k++) {
        int e = edge_of(s, s->t[t][k], s->t[t][(k + 1) % 3]);
        s->tnb[t][k] = e < 0 ? -1 : s->et[e][0] == t ? s->et[e][1] : s->et[e][0];
    }
    s->df = sqrtf(30.0f / (float)s->ne);
    return 1;
}
