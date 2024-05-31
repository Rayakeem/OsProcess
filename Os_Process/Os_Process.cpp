#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <fstream>
#include <string>

using namespace std;

// 프로세스 상태
enum class ProcessState {
    READY,
    RUNNING,
    WAITING
};

// 프로세스 종류
enum class ProcessType {
    FOREGROUND,
    BACKGROUND
};

// 프로세스 정보를 담는 구조체
struct Process {
    int pid;
    ProcessType type;
    ProcessState state;
    int remainingTime; // WAITING 상태일 때 남은 시간
};

// 스택 노드 구조체
struct StackNode {
    vector<Process*> processes;
    StackNode* next;
};

// Dynamic Queue
class DynamicQueue {
public:
    StackNode* top;
    StackNode* bottom;
    StackNode* P; // promote()를 수행할 스택 노드 포인터
    int threshold; // 리스트 분할 임계값

    DynamicQueue() : top(nullptr), bottom(nullptr), P(nullptr), threshold(0) {}

    // enqueue 함수
    void enqueue(Process* process) {
        lock_guard<mutex> lock(mtx); // 뮤텍스 잠금

        StackNode* newNode = new StackNode;
        newNode->processes.push_back(process);
        newNode->next = nullptr;

        if (process->type == ProcessType::FOREGROUND) {
            // FG 프로세스: 맨 위 스택에 추가
            if (top == nullptr) {
                top = bottom = newNode;
                P = top; // P 초기화
            }
            else {
                newNode->next = top;
                top = newNode;
            }
        }
        else {
            // BG 프로세스: 맨 아래 스택에 추가
            if (bottom == nullptr) {
                top = bottom = newNode;
                P = top; // P 초기화
            }
            else {
                bottom->next = newNode;
                bottom = newNode;
            }
        }

        updateThreshold(); // threshold 업데이트
        split_n_merge(top); // 리스트 분할 및 병합
    }

    // dequeue 함수
    Process* dequeue() {
        lock_guard<mutex> lock(mtx); // 뮤텍스 잠금

        if (top == nullptr) {
            return nullptr; // 큐가 비어있음
        }

        Process* process = top->processes.front();
        top->processes.erase(top->processes.begin());

        if (top->processes.empty()) {
            StackNode* temp = top;
            top = top->next;
            delete temp;

            if (top == nullptr) {
                bottom = nullptr; // 큐가 비어있게 됨
                P = nullptr; // P 초기화
            }
        }

        updateThreshold(); // threshold 업데이트
        return process;
    }

    // promote 함수
    void promote() {
        lock_guard<mutex> lock(mtx); // 뮤텍스 잠금

        if (P == nullptr || top == nullptr) {
            return; // promote할 스택 없음
        }

        if (!P->processes.empty()) {
            Process* process = P->processes.front();
            P->processes.erase(P->processes.begin());
            if (P->next == nullptr) {
                // 새로운 스택 노드 생성
                P->next = new StackNode;
                P->next->next = nullptr;
                bottom = P->next; // bottom 업데이트
            }
            P->next->processes.push_back(process);

            if (P->processes.empty()) {
                // P가 가리키는 스택이 비어있으면 제거
                StackNode* temp = P;
                if (P == top) {
                    top = P->next;
                }
                else {
                    StackNode* prev = top; 
                    while (prev->next != P) {
                        prev = prev->next;
                    }
                    prev->next = P->next;
                }

                if (P == bottom) {
                    bottom = prev; // bottom 업데이트
                }

                delete temp;
            }
        }

        // P 포인터 이동
        P = P->next ? P->next : top; // 시계 방향 이동

        updateThreshold(); // threshold 업데이트
        split_n_merge(top); // 리스트 분할 및 병합
    }

    // split_n_merge 함수
    void split_n_merge(StackNode* node) {
        if (node == nullptr || node->processes.size() <= threshold) {
            return; // 분할 필요 없음
        }

        int mid = node->processes.size() / 2;
        vector<Process*> front(node->processes.begin(), node->processes.begin() + mid);
        node->processes.erase(node->processes.begin(), node->processes.begin() + mid);

        if (node->next == nullptr) {
            // 새로운 스택 노드 생성
            node->next = new StackNode;
            node->next->next = nullptr;
            bottom = node->next; // bottom 업데이트
        }

        node->next->processes.insert(node->next->processes.end(), front.begin(), front.end());
        split_n_merge(node->next); // 재귀 호출
    }

private:
    // threshold 업데이트 함수
    void updateThreshold() {
        int totalProcesses = 0;
        StackNode* current = top;
        while (current != nullptr) {
            totalProcesses += current->processes.size();
            current = current->next;
        }
        int numStacks = 0;
        current = top;
        while (current != nullptr) {
            numStacks++;
            current = current->next;
        }
        threshold = totalProcesses / numStacks;
    }
};

// Wait Queue
queue<Process*> waitQueue;

// 뮤텍스 및 조건 변수
mutex mtx;
condition_variable cv;

// 프로세스 목록 (shell, monitor)
vector<Process*> processes;

// 스케줄링 함수
void scheduler(DynamicQueue& dq) {
    while (true) {
        // 1초 대기
        this_thread::sleep_for(chrono::seconds(1));

        // 뮤텍스 잠금
        unique_lock<mutex> lock(mtx);

        // Dynamic Queue에서 프로세스 실행 및 관리
        Process* process = dq.dequeue();
        if (process != nullptr) {
            process->state = ProcessState::RUNNING;
            // ... (실제 프로세스 실행 로직 구현)
            process->state = ProcessState::READY;
            dq.enqueue(process); // 다시 큐에 추가
        }

        dq.promote();
        dq.split_n_merge(dq.top);

        // Wait Queue에서 프로세스 상태 업데이트
        // ... (대기 시간 감소, READY 상태로 변경)

        // 뮤텍스 잠금 해제
        lock.unlock();

        // 조건 변수 알림
        cv.notify_all();
    }
}

// shell 프로세스 함수
void shellProcess(int pid, int delay) {
    ifstream inputFile("input.txt"); // 파일 입력

    while (inputFile) {
        // 파일에서 명령어 읽기
        string command;
        getline(inputFile, command);

        // 명령어 처리
        // ... (실제 명령어 처리 로직 구현)

        // delay 시간 동안 대기
        this_thread::sleep_for(chrono::milliseconds(delay));
    }
}

// monitor 프로세스 함수
void monitorProcess(int pid, int delay, DynamicQueue& dq) {
    while (true) {
        // 뮤텍스 잠금
        unique_lock<mutex> lock(mtx);

        // 시스템 상태 출력
        // ... (Running, DQ, WQ 상태 출력)

        // 뮤텍스 잠금 해제
        lock.unlock();

        // delay 시간 동안 대기
        this_thread::sleep_for(chrono::milliseconds(delay));
    }
}

int main() {
    // 프로세스 생성 (shell, monitor)
    Process shell = { 0, ProcessType::FOREGROUND, ProcessState::READY, 0 };
    Process monitor = { 1, ProcessType::BACKGROUND, ProcessState::READY, 0 };
    processes.push_back(&shell);
    processes.push_back(&monitor);

    DynamicQueue dq; // DynamicQueue 객체 생성
    dq.enqueue(&shell);
    dq.enqueue(&monitor);

    // 스레드 생성 (shell, monitor, scheduler)
    thread t1(shellProcess, shell.pid, 1500);
    thread t2(monitorProcess, monitor.pid, 1000, ref(dq)); // monitor
    // scheduler 스레드
    thread t3(scheduler, ref(dq));

    t1.join();
    t2.join();
    t3.join(); // 실제로는 scheduler는 무한 루프이므로 join하지 않아야 함

    return 0;
}
