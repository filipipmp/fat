#include "fat.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ds.h"

#define SUPER 0
#define TABLE 2
#define DIR 1

// #define SIZE 1024

// the superblock
#define MAGIC_N 0xAC0010DE
typedef struct {
  int magic;
  int number_blocks;
  int n_fat_blocks;
  char empty[BLOCK_SIZE - 3 * sizeof(int)];
} super;

super sb;

// item
#define MAX_LETTERS 6
#define OK 1
#define NON_OK 0
typedef struct {
  unsigned char used;
  char name[MAX_LETTERS + 1];
  unsigned int length;
  unsigned int first;
} dir_item;

#define N_ITEMS (BLOCK_SIZE / sizeof(dir_item))
dir_item dir[N_ITEMS];

// table
#define FREE 0
#define EOFF 1
#define BUSY 2
unsigned int *fat;

int mountState = 0;

int fat_format() {
  if (mountState) {
    printf("ja montado!\n");
    return -1;
  }

  // cria o superbloco
  sb.magic = MAGIC_N;
  sb.number_blocks = ds_size();
  sb.n_fat_blocks = sb.number_blocks / 1024 + 1;
  ds_write(SUPER, (char *)&sb);

  // cria o diretorio
  // memset(dir, 0, sizeof(dir));
  ds_write(DIR, (char *)dir);

  // cria a fat
  fat = malloc(sb.n_fat_blocks * BLOCK_SIZE);

  // marcar superblock, diretorio, e os blocos da fat como ocupados
  fat[0] = BUSY; // superblock
  fat[1] = BUSY; // diretorio
  for (int i = 2; i < sb.n_fat_blocks + 2; i++) {
    fat[i] = BUSY;
  }

  ds_write(TABLE, (char *)fat);

  return 0;
}

void fat_debug() {
  super debug_sb;
  ds_read(SUPER, (char *)&debug_sb);

  printf("superblock:\n");
  if (debug_sb.magic != MAGIC_N) {
    printf("  magic is not ok!\n");
  } else {
    printf("  magic is ok\n");
    printf("  %d blocks\n", debug_sb.number_blocks);
    printf("  %d blocks fat\n", debug_sb.n_fat_blocks);
  }

  unsigned int *debug_fat;
  debug_fat = malloc(debug_sb.n_fat_blocks * BLOCK_SIZE);
  for (int i = 0; i < debug_sb.n_fat_blocks; i++) {
    ds_read(TABLE + i,
            (char *)(debug_fat + i * BLOCK_SIZE / sizeof(unsigned int)));
  }

  dir_item debug_dir[N_ITEMS];
  ds_read(DIR, (char *)debug_dir);

  for (int i = 0; i < N_ITEMS; i++) {
    if (debug_dir[i].used) {
      printf("File \"%s\":\n", debug_dir[i].name);
      printf("  size: %d bytes\n", debug_dir[i].length);

      printf("  Blocks: ");
      unsigned int block = debug_dir[i].first;
      while (block != EOFF) {
        printf("%d ", block);
        block = debug_fat[block];
      }
      printf("\n");
    }
  }

  free(debug_fat);
}

int fat_mount() {
  if (mountState) {
    printf("ja montado!\n");
    return -1;
  }

  // ler o superbloco
  ds_read(SUPER, (char *)&sb);
  if (sb.magic != MAGIC_N) {
    return -1;
  }
  // TODO: mais alguma verificacao de validez?

  // ler a fat
  fat = malloc(sb.n_fat_blocks * BLOCK_SIZE);
  for (int i = 0; i < sb.n_fat_blocks; i++) {
    ds_read(TABLE + i, (char *)(fat + i * BLOCK_SIZE / sizeof(unsigned int)));
  }

  // ler o diretorio
  ds_read(DIR, (char *)dir);

  // marcar como montado
  mountState = 1;

  return 0;
}

int fat_create(char *name) {
  if (!mountState) {
    printf("fat nao montado!\n");
    return -1;
  }

  // verifica se o nome e valido
  if (strlen(name) > MAX_LETTERS) {
    printf("nome muito grande!\n");
    return -1;
  }

  // verifica se o nome ja existe
  if (find_file(name) != -1) {
    printf("arquivo ja existe!\n");
    return -1;
  }

  // for (int i = 0; i < N_ITEMS; i++) {
  //   if (dir[i].used && !strcmp(dir[i].name, name)) {
  //     printf("arquivo ja existe!\n");
  //     return -1;
  //   }
  // }

  // procura um espaco livre no diretorio
  int free_index = -1;
  for (int i = 0; i < N_ITEMS; i++) {
    if (!dir[i].used) {
      free_index = i;
      break;
    }
  }
  if (free_index == -1) {
    printf("diretorio cheio!\n");
    return -1;
  }

  // procurar um bloco livre na fat
  unsigned int first_block = -1;
  for (unsigned int i = 0; i < sb.number_blocks; i++) {
    if (fat[i] == FREE) {
      first_block = i;
      break;
    }
  }
  if (first_block == -1) {
    printf("erro: sem blocos livres na fat!\n");
    return -1;
  }

  fat[first_block] = EOFF; // marca o primeiro bloco como fim de arquivo

  // salvar entrada no diretorio
  dir[free_index].used = 1;
  strncpy(dir[free_index].name, name, MAX_LETTERS);
  dir[free_index].name[MAX_LETTERS] = '\0';
  dir[free_index].length = 0;
  dir[free_index].first = first_block;

  // atualizar disco
  ds_write(DIR, (char *)dir);
  for (int i = 0; i < sb.n_fat_blocks; i++) {
    ds_write(TABLE + i, (char *)(fat + i * BLOCK_SIZE / sizeof(unsigned int)));
  }

  return 0;
}

int fat_delete(char *name) {
  if (!mountState) {
    printf("fat nao montado!\n");
    return -1;
  }

  // procura o arquivo no diretorio
  int index = find_file(name);
  if (index == -1) {
    printf("arquivo nao encontrado!\n");
    return -1;
  }

  // libera os blocos na fat
  unsigned int block = dir[index].first;
  while (block != EOFF) {
    unsigned int next_block = fat[block];
    fat[block] = FREE; // marca o bloco como livre
    block = next_block;
  }

  // remove a entrada do diretorio
  dir[index].used = 0;

  ds_write(DIR, (char *)dir);

  for (int i = 0; i < sb.n_fat_blocks; i++) {
    ds_write(TABLE + i, (char *)(fat + i * BLOCK_SIZE / sizeof(unsigned int)));
  }

  return 0;
}

int fat_getsize(char *name) {
  if (!mountState) {
    printf("fat nao montado!\n");
    return -1;
  }

  // procura o arquivo no diretorio
  int index = find_file(name);
  if (index == -1) {
    printf("arquivo nao encontrado!\n");
    return -1;
  }

  return dir[index].length;
}

// Retorna a quantidade de caracteres lidos
int fat_read(char *name, char *buff, int length, int offset) {
  if (!mountState) {
    printf("fat nao montado!\n");
    return -1;
  }

  // procura o arquivo no diretorio
  int index = find_file(name);
  if (index == -1) {
    printf("arquivo nao encontrado!\n");
    return -1;
  }

  // verifica se o tamanho e valido
  if (length <= 0 || offset < 0) {
    printf("tamanho invalido!\n");
    return -1;
  }

  // calcula o bloco inicial e o deslocamento dentro do bloco
  unsigned int block = dir[index].first;
  int bytes_read = 0;
  int bytes_to_read = dir[index].length - offset;
  int bytes_in_block;
  while (block != EOFF && bytes_read < length) {
    // se o bloco atual nao tiver dados suficientes, pula para o proximo
    if (offset >= BLOCK_SIZE) {
      block = fat[block];
      offset -= BLOCK_SIZE;
      continue;
    }
    // calcula quantos bytes ler deste bloco
    bytes_in_block = BLOCK_SIZE - offset;
    if (bytes_in_block > bytes_to_read) {
      bytes_in_block = bytes_to_read;
    }

    // lê os dados do bloco
    char block_data[BLOCK_SIZE];
    ds_read(block, block_data);

    // copia os dados para o buffer
    memcpy(buff + bytes_read, block_data + offset, bytes_in_block);

    // atualiza contadores
    bytes_read += bytes_in_block;
    bytes_to_read -= bytes_in_block;

    // vai para o próximo bloco
    block = fat[block];
    offset = 0; // no próximo bloco, começamos do início
  }

  return bytes_read;
}

// Retorna a quantidade de caracteres escritos
int fat_write(char *name, const char *buff, int length, int offset) {
  if (!mountState) {
    printf("fat nao montado!\n");
    return -1;
  }

  printf("debug: fat_write(%s, buff, %d, %d)\n", name, length, offset);
  int original_offset = offset;

  // procura o arquivo no diretorio
  int index = find_file(name);
  if (index == -1) {
    printf("arquivo nao encontrado!\n");
    return -1;
  }

  // não permite sobrescrever o arquivo
  if (dir[index].length > 0 && offset == 0) {
    printf("arquivo ja existe, nao e possivel sobrescrever!\n");
    return -1;
  }

  // calcula o bloco inicial e o deslocamento dentro do bloco
  unsigned int block = dir[index].first;
  int bytes_written = 0;
  int bytes_to_write = dir[index].length - offset;
  int bytes_in_block;
  while (block != EOFF && bytes_written < length) {
    // printf("debug: bloco %d, offset %d, bytes_written %d\n", block, offset,
    //        bytes_written);
    // se o offset não apontar para o bloco atual, pula para o próximo
    // printf("debug: offset %d, BLOCK_SIZE %d\n", offset, BLOCK_SIZE);
    if (offset >= BLOCK_SIZE) {
      offset -= BLOCK_SIZE;
      // se o bloco atual é EOFF, busca o próximo bloco livre
      if (fat[block] == EOFF) {
        printf("debug: bloco %d é EOFF, procurando próximo bloco livre\n",
               block);
        // TODO: funcao separada para achar bloco livre?
        unsigned int new_block = -1;
        for (unsigned int i = 0; i < sb.number_blocks; i++) {
          // FIXME: erro ao chegar no bloco 1023, mesmo que a fat tenha mais
          // blocos
          if (fat[i] == FREE) {
            new_block = i;
            break;
          }
        }
        if (new_block == -1) {
          printf("aviso: sem blocos livres na fat!\n");
          // em vez de retornar erro, break para calcular o tamanho do arquivo e
          // retornar bytes escritos até aqui
          break;
        }
        // printf(
        //     "debug(1): bloco %d era EOFF, agora apontando para novo bloco
        //     %d\n", block, new_block);
        fat[block] = new_block; // liga o bloco atual ao novo bloco
        fat[new_block] = EOFF;  // marca o novo bloco como fim de arquivo
      }
      block = fat[block]; // atualiza o bloco atual
      // printf("debug: pulando para o proximo bloco %d, offset %d\n", block,
      //        offset);
      continue;
    }
    // calcula quantos bytes escrever neste bloco
    bytes_in_block = BLOCK_SIZE - offset;
    if (bytes_in_block > length - bytes_written) {
      bytes_in_block = length - bytes_written;
    }

    // lê os dados do bloco
    char block_data[BLOCK_SIZE];
    ds_read(block, block_data);

    // copia os dados do buffer para o bloco
    // printf("debug: copiando %d bytes para o bloco %d, offset %d\n",
    //        bytes_in_block, block, offset);
    memcpy(block_data + offset, buff + bytes_written, bytes_in_block);

    // escreve os dados de volta no disco
    ds_write(block, block_data);

    // atualiza contadores
    bytes_written += bytes_in_block;
    offset = 0; // no próximo bloco, começamos do início

    // se ainda não terminamos de escrever, precisamos de mais blocos
    if (bytes_written < length) {
      if (fat[block] == EOFF) {
        // procurar um novo bloco livre na fat
        unsigned int new_block = -1;
        for (unsigned int i = 0; i < sb.number_blocks; i++) {
          if (fat[i] == FREE) {
            new_block = i;
            break;
          }
        }
        if (new_block == -1) {
          printf("aviso: sem blocos livres na fat!\n");
          // em vez de retornar erro, break para calcular o tamanho do arquivo e
          // retornar bytes escritos até aqui
          break;
        }
        // printf(
        //     "debug(2): bloco %d era EOFF, agora apontando para novo bloco
        //     %d\n", block, new_block);
        fat[block] = new_block; // liga o bloco atual ao novo bloco
        fat[new_block] = EOFF;  // marca o novo bloco como fim de arquivo
      }
      block = fat[block]; // vai para o próximo bloco
    }
  }

  // atualiza o tamanho do arquivo no diretório
  printf("debug: bytes_written %d, offset %d\n", bytes_written, offset);
  // if (new_length > dir[index].length) {
  dir[index].length = original_offset + bytes_written;
  printf("debug: atualizando tamanho do arquivo: %d\n", dir[index].length);
  ds_write(DIR, (char *)dir);
  // }

  // salvar a fat
  for (int i = 0; i < sb.n_fat_blocks; i++) {
    ds_write(TABLE + i, (char *)(fat + i * BLOCK_SIZE / sizeof(unsigned int)));
  }

  return bytes_written;
}

int find_file(char *name) {
  for (int i = 0; i < N_ITEMS; i++) {
    if (dir[i].used && !strcmp(dir[i].name, name)) {
      return i;
    }
  }
  return -1; // arquivo não encontrado
}
