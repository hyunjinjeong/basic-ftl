#include <stdio.h>
#include <stdlib.h>


// Macros, structures, global variables

#define pageSize 32 // 32 LBA(1 LBA의 크기는 512Byte)가 하나의 페이지를 차지함.

typedef struct _Node { // struct Node
	int blkNumber;
	int pageCnt;
	int validCnt;

	struct _Node *prev;
	struct _Node *next;
} Node;

typedef struct _List { // struct doubly-linked list
	int cnt;
	Node *head;
	Node *tail;
} List;


List *freeBlkList = NULL; // free block들의 리스트
List *unfreeBlkList = NULL; // unfree block들의 리스트
Node *openBlk = NULL; // 현재 open 되어있는 block.
int L2P[1024*2098]; // 총 블락이 2048개, 블락당 페이지가 1024개.. 하나당 4Byte이므로 table의 크기는 총 크기는 8MB. Logical -> Physical임. 나머지 50개는 overflow division 영역!
int P2L[1024*2098]; // Physical->Logical. 블럭, 페이지는 순서대로. 0~1023번은 슈퍼블락0번, 1024~2047은 슈퍼블락1번..

//
// List functions
//

List *create_list() { // 빈 리스트를 생성.
	List *L = (List *)malloc(sizeof(List));

	L->cnt = 0;
	L->head = L->tail = NULL;

	return L;
}

Node *find_node_with_blkNum(List *L, int num) { // block Number를 가지고 원하는 block을 찾음.
	Node *N = L->head;

	if(openBlk != NULL && openBlk->blkNumber == num) {
		return openBlk;
	}

	for(N = L->head; N != NULL; N = N->next) {
		if(N->blkNumber == num) {
			return N;
		}
	}

	return NULL;
}

void add_list_tail(List *L, int newblk) { // list의 tail에 item 삽입(queue 연산)
	Node *N = (Node *)malloc(sizeof(Node));
	
	N->blkNumber = newblk;
	N->pageCnt = 0;
	N->validCnt = 0;
	N->next = NULL;

	if(L->cnt == 0) {
		L->head = L->tail = N;
	}
	else {
		N->prev = L->tail;
		L->tail->next = N;
		L->tail = N;
	}
	L->cnt++;
}

void add_list_tail_node(List *L, Node *N) { // list의 tali에 node 삽입(queue 연산)
	N->next = NULL;

	if(L->cnt == 0) {
		L->head = L->tail = N;
	}
	else {
		N->prev = L->tail;
		L->tail->next = N;
		L->tail = N;
	}
	L->cnt++;
}

Node *remove_head(List *L) { // list의 head item을 삭제(queue 연산)
	if(L->cnt == 0) {
		printf("ERROR: There is no free block.\n");
		return NULL;		
	}
	else if(L->cnt == 1) {
		Node *tmp = L->head;

		L->head = L->tail = NULL;
		L->cnt--;
		return tmp;
	}
	else {
		Node *tmp = L->head;
		
		tmp->next->prev = NULL;
		L->head = tmp->next;
		tmp->next = NULL;
		L->cnt--;
		return tmp;
	}
}

Node *remove_node(List *L, Node *N) { // list의 원하는 node를 삭제
	if(N == L->head && N == L->tail) { // 지울 노드가 head=tail일 경우
		L->head = L->tail = NULL;
		L->cnt--;
		return N;
	}
	else if(N == L->head) { // head일 경우
		N->next->prev = NULL;
		L->head = N->next;
		N->next = NULL;
		L->cnt--;
		return N;
	}
	else if(N == L->tail) { //tail인 경우
		N->prev->next = NULL;
		L->tail = N->prev;
		N->prev = NULL;
		L->cnt--;
		return N;
	}
	else { // 둘다 아닐 경우
		N->prev->next = N->next;
		N->next->prev = N->prev;

		N->prev = N->next = NULL;
		L->cnt--;
		return N;	
	}
}

Node *delete_item_with_validCnt(List *L, int item) { // list의 원하는 item을 삭제.
	Node *tmp;

	for(tmp = L->head; tmp != NULL; tmp = tmp->next) {
		if(tmp->validCnt == item) {
			if(tmp == L->head && tmp == L->tail) { // 이 경우는 head=tail, 즉 1개의 item이 있는 경우. 
				L->head = L->tail = NULL;
				L->cnt--;
				return tmp;
			}
			else if(tmp == L->head) {
				// 지울 node가 head인 경우.
				tmp->next->prev = NULL;
				L->head = tmp->next;
				tmp->next = NULL;
				L->cnt--;
				return tmp;
			}
			else if(tmp == L->tail) { // tail인 경우.
				tmp->prev->next = NULL;
				L->tail = tmp->prev;
				tmp->prev = NULL;
				L->cnt--;
				return tmp;
			}
			else { // head도, tail도 아닌 경우
				tmp->prev->next = tmp->next;
				tmp->next->prev = tmp->prev;

				tmp->prev = tmp->next = NULL;
				L->cnt--;
				return tmp;
			}
		}
	}
	return NULL;
}


//
// End List
//


// Write는 well align, misalign으로 나누고 각각 케이스에서 1 페이지만 쓰는 경우, 여러 페이지를 쓰는 경우로 나눔.
// 이미 쓰여진 mapping이 있으면 기본적으로 위의 경우와 비슷하나 다른 점은 read-back 후 overwrite.
// misalign의 경우는 첫 페이지를 read-back을 한 뒤, mapping을 덮어 씌움.

// Read는 한 페이지를 읽는 경우, 여러 페이지를 읽는 경우로 나눔.

// Erase는 요청한 페이지의 맵핑을 invalid로 만듬(0xFFFFFFFF). 마찬가지로 한 페이지를 erase하는 경우, 여러 페이지를 erase하는 경우로 나뉘어짐.

// Garbage Collection은 free block의 수가 20개 이하일 때 실행해서 40개가 될 때까지 실행한다.
// unfreeblock 중에서 valid page의 갯수가 가장 적은 block들을 찾아서 victim block으로 선정한 후,
// free block에 victim block의 valid page들을 복사하고, victim block들은 다시 free block이 되어 write 할 수 있게 된다.


void GarbageCollection() { 
	int i;
	printf("Garbage Collection request\n");
	
	while(freeBlkList->cnt < 40) { // 40개가 될 때까지 Garbage Collection 수행.
		Node *N = unfreeBlkList->head;
		Node *victim = N;

		// victim이 될 block을 찾는다.
		while(N != NULL) {
			if(N->validCnt < victim->validCnt) {
				victim = N;
			}
			N = N->next;
		}

		if(victim == NULL) { // 모든 block이 free일 경우 victim block이 없음.(이런 경우는 발생하지 않겠지만..)
			printf("[G.C.] There is no unfree block.\n");
			return;
		}

		printf("[G.C.] Victim block is (superBlock %d), and the number of valid page is %d\n", victim->blkNumber, victim->validCnt);

		if(victim->validCnt > 0) { // 모든 page가 invalid인 경우 바로 free block으로 넘어가야 함.
			for(i = 1024*(victim->blkNumber); i < 1024+1024*(victim->blkNumber); i++) { // victim block의 페이지 전체를 다 돌면서 맵핑을 옮겨줌.
				if(openBlk->pageCnt == 1024) { // block이 꽉 차면 다음 블락으로 넘어감.
					add_list_tail_node(unfreeBlkList, openBlk);
					openBlk = remove_head(freeBlkList);
				}

				if(openBlk == NULL) { // 더 이상 free block이 없을 경우.
					printf("[G.C.] There is no free block..\n");
					return;
				}
		
				// P2L[i]는 physical이 어디에 맵핑되어 있는지를 알려줌. 따라서 P2L[i]를 L2P에 넣으면 L2P[P2L[i]]는 L2P 정보가 나오게 되고, 이 맵핑을
				// 새로 뽑은 openBlk으로 옮겨 줌. 그 뒤, openBlk이 맵핑이 되므로 openBlk의 P2L에도 어디에 L2P 맵핑이 되어 있는지 정보를 알려줘야 함.

				if(P2L[i] != -1) { // -1이면 맵핑이 안 되어 있다, 즉 invalid라는 뜻이므로 넣어주면 안 됨.
					if(L2P[P2L[i]] == i) { // 이 경우에만 valid함. 만약 같지 않다면 다른 physical block에서 logical을 overwrite했다는 것이므로 invalid라는 뜻.
						printf("[G.C.] In L2P[%d], (superBlock %d, superPage %d) -> (superBlock %d, superPage %d)\n", P2L[i], L2P[P2L[i]]/1024, L2P[P2L[i]]%1024, openBlk->blkNumber, openBlk->pageCnt);
						L2P[P2L[i]] = 1024*openBlk->blkNumber + openBlk->pageCnt;
						P2L[1024*openBlk->blkNumber + openBlk->pageCnt] = P2L[i];
						openBlk->pageCnt++;
						openBlk->validCnt++;
					}
				}
			}
		}
	
		//mapping을 다 옮겼으므로 다시 free block list에 넣어줌.
		victim->pageCnt = 0;
		victim->validCnt = 0;
		remove_node(unfreeBlkList, victim); // unfree block list에서 빼줌
		add_list_tail_node(freeBlkList, victim); // 다시 free block list로 넣어줌.

		if(openBlk->pageCnt == 1024) { // block이 꽉 차면 다음 블락으로 넘어감.
			add_list_tail_node(unfreeBlkList, openBlk);
			openBlk = remove_head(freeBlkList);
		}
	}
	printf("\n");
}

void Erase(int startAddr, int endAddr) {
	int mapsAddr = startAddr/pageSize;
	int mapeAddr = endAddr/pageSize;

	printf("Erase request to address 0x%x-0x%x\n", startAddr, endAddr);

	if(startAddr >= pageSize*1024*2048 || endAddr >= pageSize*1024*2048) {
		printf("Error: User cannot access this address\n");
		return;
	}

	if(mapsAddr == mapeAddr) { // 한 페이지만 지울 경우.
		if(L2P[mapsAddr] != 0xFFFFFFFF) { // 데이터가 있을 경우만.. 0xFFFFFFFF일 때는 X.
			find_node_with_blkNum(unfreeBlkList, L2P[mapsAddr]/1024)->validCnt--;
		}
		L2P[mapsAddr] = 0xFFFFFFFF;
		printf("[ERASE] Now L2P[%d] is invalid\n\n", mapsAddr);
	}
	else { // 여러 페이지를 지워야 할 경우
		int i;

		for(i = mapsAddr; i <= mapeAddr; i++) {
			if(L2P[i] != 0xFFFFFFFF) {
				find_node_with_blkNum(unfreeBlkList, L2P[i]/1024)->validCnt--;
			}
			L2P[i] = 0xFFFFFFFF;
			printf("[ERASE] Now L2P[%d] is invalid\n", i);
		}
		printf("\n");
	}
}

void Read(int startAddr, int chunk) { // Logical Address와 크기를 받아 맵핑정보를 출력.
	int mapAddr = startAddr/pageSize;

	printf("Read request to address 0x%x, chunk %d\n", startAddr, chunk);

	if(chunk > 0)
		chunk--;

	if(startAddr+chunk >= pageSize*1024*2048) {
		printf("Error: User cannot access this address.\n");
		return;
	}

	if(mapAddr == (startAddr+chunk)/pageSize) { // 한 페이지만 읽는 경우.
		if(L2P[mapAddr] == 0xFFFFFFFF)
			printf("[READ] L2P[%d] is invalid\n", mapAddr);
		else
			printf("[READ] L2P[%d] (superBlock %d, superPage %d)\n", mapAddr, L2P[mapAddr]/1024, L2P[mapAddr]%1024);
	}
	else { // 여러 페이지를 읽어야 할 경우.
		int chunkNumber = (startAddr+chunk)/pageSize - mapAddr;
		
		while(chunkNumber >= 0) {
			if(L2P[mapAddr] == 0xFFFFFFFF) 
				printf("[READ] L2P[%d] is invalid\n", mapAddr);
			else
				printf("[READ] L2P[%d] (superBlock %d, superPage %d)\n", mapAddr, L2P[mapAddr]/1024, L2P[mapAddr]%1024);
			
			mapAddr++;
			chunkNumber--;
		}
	}
	printf("\n");
}

void Write(int startAddr, int chunk) {
	// 하나의 superblk은 1024개의 superpage가 있다고 가정. page는 16KB.
	// mapping table은 4Byte(32bit) 중 오른쪽 10자리(0~9)가 페이지 번호, 그 다음 11자리(10~20)가 블락 번호.
	// 따라서 table entry에 넣을 값은 page + 1024*blockNumber.
	
	int mapAddr = startAddr/pageSize; // 테이블의 몇 번째 위치에 넣을 지 결정.

	printf("Write request to address 0x%x, chunk %d\n", startAddr, chunk);

	if(chunk > 0)
		chunk--;

	if(startAddr+chunk >= pageSize*1024*2048) {
		printf("Error: User cannot access this address.\n");
		return;
	}

	if(openBlk == NULL || openBlk->pageCnt == 1024) { // write를 처음 하거나, block이 꽉 찬 경우 queue에서 new free block을 새로 뽑음.
		if(openBlk != NULL && openBlk->pageCnt == 1024) // openBlk가 다 쓰여졌으므로 unfreeBlock Queue에 넣는다.
			add_list_tail_node(unfreeBlkList, openBlk);
		
		openBlk = remove_head(freeBlkList);
		if(freeBlkList->cnt <= 20) // free block의 수가 20개 이하면 garbage collection.
			GarbageCollection();
	}

	if(openBlk == NULL) // 여기서는 위에어 remove_queue로 openBlk을 뽑았는데 NULL인 경우. 즉 free block이 없는 경우이므로 바로 리턴.
		return;

	if((startAddr % pageSize) == 0) { // well aligned case.
		if(chunk < pageSize) { // chunk가 페이지 사이즈보다 같거나 작은 경우 그 페이지에 모두 쓸 수 있음.
			if(L2P[mapAddr] != 0xFFFFFFFF) { // 기존 mapping이 있는 경우 read-back.
				printf("[WRITE] Read-back\n");
				Read(startAddr, pageSize);
				find_node_with_blkNum(unfreeBlkList, L2P[mapAddr]/1024)->validCnt--; // validCnt--;
			}
			L2P[mapAddr] = openBlk->pageCnt + 1024*openBlk->blkNumber; // 맵핑해줌.
			P2L[L2P[mapAddr]] = mapAddr;
			printf("[WRITE] 0x%x: Write to L2P[%d] (superBlock %d, superPage %d)\n", startAddr, mapAddr, L2P[mapAddr]/1024, L2P[mapAddr]%1024);
			openBlk->pageCnt++;
			openBlk->validCnt++;
			printf("\n");
		}
		else { // 아닐 경우 여러 페이지에 써야 함.
			int chunkNumber = chunk/pageSize; // 0~31까지 0, 32~63까지 1, 64~95까지 2...
			int cnt = 0;
			
			while(chunkNumber >= 0) {
				if(openBlk->pageCnt == 1024) { // block이 꽉 차면 다음 블락으로.
					add_list_tail_node(unfreeBlkList, openBlk);
					openBlk = remove_head(freeBlkList);

					if(freeBlkList->cnt <= 20)
						GarbageCollection();
				}
				
				if(openBlk == NULL) { // free block이 없을 경우 return.
					return;
				}

				if(L2P[mapAddr] != 0xFFFFFFFF) { 
					if(chunkNumber == 0) { // 여러 페이지를 쓸 때 마지막에 mapping이 있는 경우 read-back.
						printf("[WRITE] Read-back\n");
						Read(startAddr+cnt*pageSize, pageSize);
					}
					find_node_with_blkNum(unfreeBlkList, L2P[mapAddr]/1024)->validCnt--; // invalid가 늘어나므로 --해줌.
				}

				L2P[mapAddr] = openBlk->pageCnt + 1024*openBlk->blkNumber;
				P2L[L2P[mapAddr]] = mapAddr;
				printf("[WRITE] 0x%x: Write to L2P[%d] (superBlock %d, superPage %d)\n", startAddr+cnt*pageSize, mapAddr, L2P[mapAddr]/1024, L2P[mapAddr]%1024);
				openBlk->pageCnt++;
				openBlk->validCnt++;
				mapAddr++;
				chunkNumber--;
				cnt++;
				printf("\n");
			}
		}
	}
	else { // misaligned case의 경우 read-back을 한 다음 새로 mapping을 해야 함.
		printf("[WRITE] Read-back\n");

		Read(mapAddr*pageSize, pageSize); // read-back
		if(L2P[mapAddr] != 0xFFFFFFFF) 
			find_node_with_blkNum(unfreeBlkList, L2P[mapAddr]/1024)->validCnt--;

		if(mapAddr == (startAddr+chunk)/pageSize) { // 한 페이지만 쓸 경우, 그냥 read-back 후 write.
			L2P[mapAddr] = openBlk->pageCnt + 1024*openBlk->blkNumber;
			P2L[L2P[mapAddr]] = mapAddr;
			
			printf("[WRITE] 0x%x: Write to L2P[%d] (superBlock %d, superPage %d)\n", mapAddr*pageSize, mapAddr, L2P[mapAddr]/1024, L2P[mapAddr]%1024);
			openBlk->pageCnt++;
			openBlk->validCnt++;
			printf("\n");
		}
		else { // 여러 페이지에 쓸 경우, 처음 페이지만 read-back 후 필요한 페이지 수만큼 write.
			int chunkNumber = (startAddr+chunk)/pageSize - mapAddr;

			while(chunkNumber >= 0) {
				if(openBlk->pageCnt == 1024) { // block이 꽉 차면 다음 블락으로.
					add_list_tail_node(unfreeBlkList, openBlk);
					openBlk = remove_head(freeBlkList);

					if(freeBlkList->cnt <= 20)
						GarbageCollection();
				}
				
				if(openBlk == NULL) { // free block이 없는 경우 return.
					return;
				}

				if(L2P[mapAddr] != 0xFFFFFFFF) { // 여러 페이지를 쓸 때 마지막에 기존 mapping이 있는 경우 read-back.
					if(chunkNumber == 0) {
						printf("[WRITE] Read-back\n");
						Read(mapAddr*pageSize, pageSize);
					}
					find_node_with_blkNum(unfreeBlkList, L2P[mapAddr]/1024)->validCnt--;
				}

				L2P[mapAddr] = openBlk->pageCnt + 1024*openBlk->blkNumber;
				P2L[L2P[mapAddr]] = mapAddr;
				printf("[WRITE] 0x%x: Write to L2P[%d] (superBlock %d, superPage %d)\n", mapAddr*pageSize, mapAddr, L2P[mapAddr]/1024, L2P[mapAddr]%1024);
				openBlk->pageCnt++;
				openBlk->validCnt++;
				mapAddr++;
				chunkNumber--;
				printf("\n");
			}
		}
	}
}

int main() {
	int i;
	Node *tmp;
	freeBlkList = create_list();
	unfreeBlkList = create_list();

	for( i = 0; i < 2098; i++ ) { //superblk의 크기는 16MB이고 flash의 크기는 32GB이므로 block이 총 2048개 필요함. overflow가 났을 때를 대비한 overflow division block이 50개 있음.
		add_list_tail(freeBlkList, i);
	}

	for(i = 0; i < 1024*2098; i++) { 
		L2P[i] = 0xFFFFFFFF; // 처음에 mapping table을 초기화. 값이 0xFFFFFFFF일 경우 아직 쓰지 않은 곳, 다른 수일 경우 이미 쓴 곳임을 알 수 있음.
		P2L[i] = -1; // P2L의 경우는 맵핑이 되어으면 L2P 배열의 어디에 맵핑되어 있는지 값이 들어가 있고, 안 되어 있으면 -1.(이것도 0xFFFFFFFF임.)
	}
	
	// test 공간. 유저는 0~2047까지의 block만 접근 가능.
	
	Write(0, pageSize*1024*2048);
	Write(0, pageSize*1024*30);
	Write(pageSize*1024*77, pageSize*1024*20);
	Write(pageSize*1024*119, pageSize*1024*120);

	tmp = freeBlkList -> head;
	while(tmp != NULL) {
		printf("block %d is free\n", tmp->blkNumber);
		tmp = tmp->next;
	}

	return 0;
}