
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    unsigned int *opaque;
} sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_wait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *value);
int sem_destroy(sem_t *sem);

#ifdef __cplusplus
}
#endif