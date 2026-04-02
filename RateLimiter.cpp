#include<iostream>
#include<future>
#include<mutex>
#include<atomic>
#include<string>
#include<map>
#include<vector>
#include<future>

//RateLimiting Strategy Interface
class RateLimitingStrategy {
public:
	virtual ~RateLimitingStrategy() = default;
	virtual bool allowRequest(const std::string& userId) = 0;
};

//FixedWindowStrategy
class UserRequestInfo {
public:
	long windowStart;
	std::atomic<int> requestCount;
	std::mutex mtx;

	UserRequestInfo(long startTime) :windowStart{ startTime }, requestCount{ 0 } {}

	void reset(long newStart) {
		windowStart = newStart;
		requestCount.store(0); 
	}
};

class FixedWindowStrategy : public RateLimitingStrategy {
private:
	int maxRequests;
	long windowSizeInMillis;
	std::map<std::string, UserRequestInfo*> userRequestMap;
	std::mutex mapMutex;
public:
	FixedWindowStrategy(int maxRequests, long windowSizeInSeconds):
		maxRequests{ maxRequests }, windowSizeInMillis(windowSizeInSeconds * 1000){}

	~FixedWindowStrategy() {
		for (auto& pair : userRequestMap) {
			delete pair.second;
		}
	}

	bool allowRequest(const std::string& userId) override {
		auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		
		std::lock_guard<std::mutex> mapLock(mapMutex);
		if (userRequestMap.find(userId) == userRequestMap.end()) {
			userRequestMap[userId] = new UserRequestInfo(currentTime);
		}

		UserRequestInfo* requestInfo = userRequestMap[userId];

		std::lock_guard<std::mutex> infoLock(requestInfo->mtx);
		if (currentTime - requestInfo->windowStart >= windowSizeInMillis) {
			requestInfo->reset(currentTime);
		}

		if (requestInfo->requestCount.load() < maxRequests) {
			requestInfo->requestCount.fetch_add(1);
			return true;
		}
		else {
			return false;
		}
	}
};
// TokenBucketRateLimiter
class TokenBucket {
public:
	int tokens;
	int capacity;
	int refillRatePerSecond;
	long lastRefillTimestamp;
	std::mutex mtx;

	TokenBucket(int capacity, int refillRatePerSecond, long currentTimeMillis) :
		capacity{ capacity }, refillRatePerSecond{ refillRatePerSecond },
		tokens{ capacity }, lastRefillTimestamp{ currentTimeMillis }
	{
	}

	void refill(long currentTime) {
		long elapsedTime = currentTime - lastRefillTimestamp;
		int tokensToAdd = static_cast<int>((elapsedTime / 1000.0) * refillRatePerSecond);

		if (tokensToAdd > 0) {
			tokens = std::min(capacity, tokens + tokensToAdd);
			lastRefillTimestamp = currentTime;
		}
	}
};

class TokenBucketRateLimiter : public RateLimitingStrategy
{
private:
	int capacity;
	int refillRatePerSecond;
	std::map<std::string, TokenBucket*> userBuckets;
	std::mutex mapMtx;
public:
	TokenBucketRateLimiter(int capacity, int refillRatePerSecond) :
		capacity{ capacity }, refillRatePerSecond{ refillRatePerSecond } {
	}

	~TokenBucketRateLimiter() {
		for (auto& pair : userBuckets) {
			delete pair.second;
		}
	}

	bool allowRequest(const std::string& userId) override {
		auto currentTime = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();

		std::lock_guard<std::mutex> mapLock(mapMtx);

		if (userBuckets.find(userId) == userBuckets.end()) {
			userBuckets[userId] = new TokenBucket(capacity, refillRatePerSecond, currentTime);

		}

		TokenBucket* bucket = userBuckets[userId];
		std::lock_guard<std::mutex> bucketLock(bucket->mtx);

		bucket->refill(currentTime);
		if (bucket->tokens > 0) {
			bucket->tokens--;
			return true;
		}
		else {
			return false;
		}
	}
};

class RateLimiterService {
private:
	static RateLimiterService* instance;
	static std::mutex instanceMutex;
	RateLimitingStrategy* rateLimitingStrategy;

	RateLimiterService() : rateLimitingStrategy{ nullptr } {}

public:
	static RateLimiterService* getInstance() {
		if (instance == nullptr) {
			std::lock_guard<std::mutex> lock(instanceMutex);
			if (instance == nullptr) {
				instance = new RateLimiterService();
			}
		}
		return instance;
	}

	void setRateLimiter(RateLimitingStrategy* rateLimitingStrategy) {
		this->rateLimitingStrategy = rateLimitingStrategy;
	}

	void handleRequest(const std::string& userId) {
		if (rateLimitingStrategy->allowRequest(userId)) {
			std::cout << "Request from user " << userId << " is allowed " << std::endl;
		}
		else {
			std::cout << "Request from user " << userId << " is rejected: Rate limit exceeded" << std::endl;
		}
	}
};
RateLimiterService* RateLimiterService::instance = nullptr;
std::mutex RateLimiterService::instanceMutex;

class RateLimiterDemo {
public:
	static void main() {
		std::string userId = "user123";
		std::cout << "---- FIXED WINDOW DEMO ----" << std::endl;
		runFixedWindowDemo(userId);

		std::cout << "\n--- TOKEN BUCKET DEMO ----" << std::endl;
		runTokenBucketDemo(userId);
	}
private:
	static void runFixedWindowDemo(const std::string& userId) {
		int maxRequests = 5;
		int windowSeconds = 10;

		RateLimitingStrategy* rateLimiter = new FixedWindowStrategy(maxRequests, windowSeconds);
		RateLimiterService* service = RateLimiterService::getInstance();
		service->setRateLimiter(rateLimiter);

		std::vector<std::future<void>> futures;

		for (int i = 0; i < 10; ++i) {
			futures.push_back(std::async(std::launch::async, [service, userId]() {
				service->handleRequest(userId);
				}));
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
		for (auto& future : futures) {
			future.wait();

		}
		delete rateLimiter;
	}

	static void runTokenBucketDemo(const std::string& userId) {
		int capacity = 5;
		int refillRate = 1;// 1 token per second

		RateLimitingStrategy* tokenBucketLimiter = new TokenBucketRateLimiter(capacity, refillRate);
		RateLimiterService* service = RateLimiterService::getInstance();
		service->setRateLimiter(tokenBucketLimiter);

		std::vector<std::future<void>> futures;

		for (int i = 0; i < 10; ++i) {
			futures.push_back(std::async(std::launch::async, [service, userId]() {
				service->handleRequest(userId);
				}));
			std::this_thread::sleep_for(std::chrono::microseconds(300));
		}
		for (auto& future : futures) {
			future.wait();

		}
		delete tokenBucketLimiter;
	}
};

int main() {
	RateLimiterDemo::main();
}
