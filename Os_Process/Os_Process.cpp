#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <fstream>
#include <string>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <chrono> 

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

    // Wait Queue에서 사용할 비교 연산자 (깨어날 시간 오름차순 정렬)
    bool operator<(const Process& other) const {
        return remainingTime > other.remainingTime; // min-heap 구현
    }
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

    // 뮤텍스
    mutex mtx;

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
priority_queue<Process*> waitQueue;

// 뮤텍스 및 조건 변수
mutex mtx;
condition_variable cv;

// 프로세스 목록 (shell, monitor)
vector<Process*> processes;
int nextPid = 0; // 다음 프로세스 PID

// 스케줄링 함수
void scheduler(DynamicQueue& dq) {
    while (true) {
        this_thread::sleep_for(chrono::seconds(1));
        unique_lock<mutex> lock(mtx);

        // Wait Queue에서 깨어날 프로세스 처리
        while (!waitQueue.empty() && waitQueue.top()->remainingTime <= 0) {
            Process* process = waitQueue.top();
            waitQueue.pop();
            process->state = ProcessState::READY;
            dq.enqueue(process);
        }

        // Dynamic Queue에서 프로세스 실행 및 관리
        Process* runningProcess = dq.dequeue();
        if (runningProcess != nullptr) {
            runningProcess->state = ProcessState::RUNNING;
            lock.unlock(); // 실행 중에는 뮤텍스 해제

            // ... (실제 프로세스 실행 로직 구현)

            lock.lock(); // 실행 후 다시 뮤텍스 잠금
            runningProcess->state = ProcessState::READY;
            dq.enqueue(runningProcess);
        }

        dq.promote();
        dq.split_n_merge(dq.top);

        // Wait Queue에서 대기 시간 감소
        priority_queue<Process*> tempQueue;
        while (!waitQueue.empty()) {
            Process* process = waitQueue.top();
            waitQueue.pop();
            process->remainingTime--;
            tempQueue.push(process);
        }
        waitQueue = move(tempQueue);

        lock.unlock();
        cv.notify_all();
    }
}

// 명령어 파싱 함수
vector<string> parse(const string& command) {
    vector<string> tokens;
    istringstream iss(command);
    string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// 프로세스 실행 함수
void executeProcess(Process* process, const vector<string>& args) {
    auto startTime = chrono::high_resolution_clock::now();
    int duration = 500; // 기본 실행 시간 (500초)
    int period = -1; // 반복 주기 (-1은 반복 없음)
    int numThreads = 1; // 병렬 처리 스레드 수

    // 명령어 옵션 파싱
    for (int i = 1; i < args.size(); i++) {
        // ... (옵션 파싱 로직은 동일)
    }

    do {
        if (args[0] == "echo") {
            cout << args[1] << endl;
        }
        else if (args[0] == "dummy") {
            // 아무 작업도 하지 않음
        }
        else if (args[0] == "gcd") {
            int x = stoi(args[1]), y = stoi(args[2]);
            cout << "gcd(" << x << ", " << y << ") = " << gcd(x, y) << endl;
        }
        else if (args[0] == "prime") {
            int x = stoi(args[1]);
            cout << "Number of primes less than or equal to " << x << ": " << countPrimes(x) << endl;
        }
        else if (args[0] == "sum") {
            int x = stoi(args[1]);
            if (numThreads == 1) {
                cout << "sum(" << x << ") % 1000000 = " << partialSum(1, x) << endl;
            }
            else {
                // 병렬 처리 (실제 구현은 생략)
                cout << "Parallel sum not implemented yet" << endl;
            }
        }

        // 실행 시간 체크 및 종료 (뮤텍스 잠금 추가)
        unique_lock<mutex> lock(mtx);
        auto endTime = chrono::high_resolution_clock::now();
        auto duration_ms = chrono::duration_cast<chrono::milliseconds>(endTime - startTime).count();
        if (duration_ms >= duration * 1000) {
            break;
        }

        if (period > 0) {
            // WAITING 상태로 변경 및 waitQueue에 추가
            process->state = ProcessState::WAITING;
            process->remainingTime = period;
            waitQueue.push(process);
            lock.unlock();
            cv.wait_for(lock, chrono::seconds(period)); // period 동안 대기
            continue; // 다음 반복으로 이동
        }

        lock.unlock();
    } while (period > 0);

    // 프로세스 종료 처리
    unique_lock<mutex> lock(mtx);
    process->state = ProcessState::READY;
    lock.unlock();
    cv.notify_all();
}

// shell 프로세스 함수
void shellProcess(int pid, int delay, DynamicQueue& dq) { // dq 참조 추가
    ifstream inputFile("input.txt");

    while (inputFile) {
        string command;
        getline(inputFile, command);
        vector<string> args = parse(command);

        if (!args.empty()) {
            // 새로운 프로세스 생성 및 실행
            Process* newProcess = new Process{ nextPid++, ProcessType::FOREGROUND, ProcessState::READY, 0 };
            processes.push_back(newProcess);

            dq.enqueue(newProcess); // 뮤텍스 잠금 없이 enqueue 호출

            thread processThread(executeProcess, newProcess, args); // 프로세스 정보 전달
            processThread.detach(); // 백그라운드 실행
        }

        this_thread::sleep_for(chrono::milliseconds(delay));
    }
}

void monitorProcess(int pid, int delay, DynamicQueue& dq) {
    while (true) {
        // 뮤텍스 잠금
        unique_lock<mutex> lock(dq.mtx); // dq 객체의 멤버 mtx 사용

        // 시스템 상태 출력
        cout << "Running: ";
        for (Process* p : processes) {
            if (p->state == ProcessState::RUNNING) {
                cout << "[" << p->pid << (p->type == ProcessType::FOREGROUND ? "F" : "B") << "] ";
            }
        }
        cout << endl;

        cout << "DQ:" << endl;
        StackNode* currentStack = dq.top;
        while (currentStack != nullptr) {
            cout << "[";
            for (Process* p : currentStack->processes) {
                cout << p->pid << (p->type == ProcessType::FOREGROUND ? "F" : "B") << " ";
            }
            cout << "] ";
            if (currentStack == dq.P) {
                cout << "P => ";
            }
            cout << endl;
            currentStack = currentStack->next;
        }

        cout << "WQ:" << endl;
        priority_queue<Process*> tempQueue = waitQueue; // 큐 복사
        while (!tempQueue.empty()) {
            Process* p = tempQueue.top();
            tempQueue.pop();
            cout << "[" << p->pid << (p->type == ProcessType::FOREGROUND ? "F" : "B") << ":" << p->remainingTime << "] ";
        }
        cout << endl << endl;

        // 뮤텍스 잠금 해제
        lock.unlock();

        // delay 시간 동안 대기
        this_thread::sleep_for(chrono::milliseconds(delay));
    }
}

// 유클리드 호제법 (최대공약수 계산)
int gcd(int a, int b) {
    if (b == 0) {
        return a;
    }
    return gcd(b, a % b);
}

// 에라토스테네스의 체 (소수 개수 계산)
int countPrimes(int n) {
    if (n <= 1) {
        return 0;
    }

    vector<bool> isPrime(n + 1, true);
    isPrime[0] = isPrime[1] = false;
    for (int i = 2; i <= sqrt(n); i++) {
        if (isPrime[i]) {
            for (int j = i * i; j <= n; j += i) {
                isPrime[j] = false;
            }
        }
    }

    return count(isPrime.begin(), isPrime.end(), true);
}

// 부분 합 계산 함수
int partialSum(int start, int end) {
    return accumulate(start, end + 1, 0LL) % 1000000; // long long 사용하여 오버플로 방지
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
    t3.join();

    return 0;
}
