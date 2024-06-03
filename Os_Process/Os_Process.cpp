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
#include <atomic>
#include <future>


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
    string command; // 프로세스 명령어

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

    void promote() {
        lock_guard<mutex> lock(mtx);

        if (P == nullptr || top == nullptr) {
            return;
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
                StackNode* current = top; // current 변수 선언 및 초기화
                StackNode* temp = P;
                if (P == top) {
                    top = P->next;
                }
                else {
                    // prev 탐색 (수정된 부분)
                    while (current && current->next != P) {
                        current = current->next;
                    }

                    if (current) { // current가 nullptr이 아닌 경우에만 next 변경
                        current->next = P->next;
                    }
                }

                // bottom 업데이트 (P가 bottom인 경우에만)
                if (P == bottom) {
                    bottom = current; // current는 이제 P의 이전 노드를 가리킴
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

        size_t mid = node->processes.size() / 2; // size_t 타입으로 변경
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

//실행함수 선언
void executeProcess(Process* process, const vector<string>& args);

// 명령어 파싱 함수 (';' 기준으로 분리)
vector<string> parseCommands(const string& line) {
    vector<string> commands;
    istringstream iss(line);
    string command;
    while (getline(iss, command, ';')) {
        if (!command.empty()) {
            commands.push_back(command);
        }
    }
    return commands;
}

// 명령어 파싱 함수 (공백 기준으로 분리)
vector<string> parseArgs(const string& command) {
    vector<string> args;
    istringstream iss(command);
    string arg;
    while (iss >> arg) {
        args.push_back(arg);
    }
    return args;
}

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

            // 프로세스 실행 (람다 함수 사용)
            thread processThread([runningProcess]() {
                vector<string> args = parseArgs(runningProcess->command); // parseArgs 함수 호출
                executeProcess(runningProcess, args);
                });
            processThread.detach(); // 백그라운드 실행
        }
        else {
            lock.unlock(); // 실행할 프로세스가 없으면 뮤텍스 해제
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


// 프로세스 실행 함수
void executeProcess(Process* process, const vector<string>& args) {
    auto startTime = chrono::high_resolution_clock::now();
    int duration = 500; // 기본 실행 시간 (500초)
    int period = -1; // 반복 주기 (-1은 반복 없음)
    int numThreads = 1; // 병렬 처리 스레드 수

    // 명령어 옵션 파싱
    for (int i = 1; i < args.size(); i++) {
        if (args[i] == "-d") {
            duration = stoi(args[++i]);
        }
        else if (args[i] == "-p") {
            period = stoi(args[++i]);
        }
        else if (args[i] == "-n") {
            numThreads = stoi(args[++i]);
        }
        else if (args[i] == "-m") {
            numThreads = stoi(args[++i]); // -m 옵션도 스레드 수로 처리
        }
    }

    do {
        // 뮤텍스 잠금 (출력 동기화)
        unique_lock<mutex> lock(mtx);

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
                // 단일 스레드 처리
                long long sum = 0;
                for (int j = 1; j <= x; j++) {
                    sum += j;
                    sum %= 1000000;
                }
                cout << "sum(" << x << ") % 1000000 = " << sum << endl;
            }
            else {
                // 병렬 처리
                vector<future<long long>> futures;
                long long chunkSize = x / numThreads;
                long long remainder = x % numThreads;

                for (int i = 0; i < numThreads; i++) {
                    int start = i * chunkSize + 1;
                    int end = (i + 1) * chunkSize;
                    if (i == numThreads - 1) {
                        end += remainder;
                    }
                    futures.push_back(async(launch::async, [start, end]() {
                        long long sum = 0;
                        for (int j = start; j <= end; j++) {
                            sum += j;
                            sum %= 1000000;
                        }
                        return sum;
                        }));
                }

                long long totalSum = 0;
                for (auto& future : futures) {
                    totalSum += future.get();
                    totalSum %= 1000000;
                }

                cout << "sum(" << x << ") % 1000000 = " << totalSum << endl;
            }
        }

        // 실행 시간 체크 및 종료
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
        }

        lock.unlock(); // 뮤텍스 해제 (출력 후)

        if (period > 0) {
            this_thread::sleep_for(chrono::seconds(period)); // period 동안 대기
        }
    } while (period > 0);

    // 프로세스 종료 처리
    unique_lock<mutex> lock(mtx);
    // 프로세스 메모리 해제
    for (auto it = processes.begin(); it != processes.end(); ++it) {
        if (*it == process) {
            processes.erase(it);
            delete process;
            break;
        }
    }
    lock.unlock();
    cv.notify_all();
}

//shell 프로세스 함수
void shellProcess(int pid, int delay, DynamicQueue& dq) {  // delay: Y초
    ifstream inputFile("commands.txt");

    while (inputFile) {
        string line;
        getline(inputFile, line); // commands.txt에서 한 줄 읽기

        if (line.empty()) {
            continue; // 빈 줄은 무시
        }

        vector<string> commands = parseCommands(line); // 명령어 파싱

        for (const string& command : commands) {
            vector<string> args = parseArgs(command);
            if (!args.empty()) {
                ProcessType type = args[0][0] == '&' ? ProcessType::BACKGROUND : ProcessType::FOREGROUND;
                if (type == ProcessType::BACKGROUND) {
                    args[0] = args[0].substr(1); // '&' 제거
                }

                Process* newProcess = new Process{ nextPid++, type, ProcessState::READY, 0, command };
                processes.push_back(newProcess);
                newProcess->command = command; // 명령어 저장

                dq.enqueue(newProcess);

                thread processThread(executeProcess, newProcess, args); // 프로세스 정보 전달
                processThread.detach(); // 백그라운드 실행
            }
        }
        this_thread::sleep_for(chrono::milliseconds(delay)); // Y초 대기
    }
}


// monitor 프로세스 함수
void monitorProcess(int pid, int delay, DynamicQueue& dq) {
    while (true) {
        this_thread::sleep_for(chrono::milliseconds(delay)); // X초 대기

        // 뮤텍스 잠금 (출력 동기화)
        lock_guard<mutex> lock(mtx);

        // 시스템 상태 출력 (atomic하게)
        cout << "\nRunning: ";
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
                if (p == currentStack->processes.back() && currentStack == dq.P) {
                    cout << "*"; // 프로모션된 프로세스 표시
                }
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
        cout << endl;
    }
}

int main() {
    // 프로세스 생성 (shell, monitor)
    Process shell = { 0, ProcessType::FOREGROUND, ProcessState::READY, 0, "" };
    Process monitor = { 1, ProcessType::BACKGROUND, ProcessState::READY, 0, "" };
    processes.push_back(&shell);
    processes.push_back(&monitor);

    DynamicQueue dq; // DynamicQueue 객체 생성
    dq.enqueue(&shell);
    dq.enqueue(&monitor);

    // 스레드 생성 (shell, monitor, scheduler) - delay 값 조정
    thread t1(shellProcess, shell.pid, 2000, ref(dq)); // 2초마다 명령어 실행
    thread t2(monitorProcess, monitor.pid, 1000, ref(dq)); // 1초마다 상태 확인
    thread t3(scheduler, ref(dq));

    t1.join(); // 셸 프로세스 종료 대기
    t2.join(); // 모니터 프로세스 종료 대기
    t3.detach(); // 스케줄러는 무한 루프이므로 detach() 호출

    // 프로세스 메모리 해제
    for (Process* p : processes) {
        delete p;
    }

    return 0;
}
