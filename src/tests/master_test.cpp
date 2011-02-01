#include <gmock/gmock.h>

#include <mesos_exec.hpp>
#include <mesos_sched.hpp>

#include <boost/lexical_cast.hpp>

#include <local/local.hpp>

#include <master/master.hpp>

#include <slave/isolation_module.hpp>
#include <slave/process_based_isolation_module.hpp>
#include <slave/slave.hpp>

#include <tests/utils.hpp>

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using boost::lexical_cast;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::Framework;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::A;
using testing::An;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::ElementsAre;
using testing::Ne;
using testing::Return;
using testing::SaveArg;
using testing::Sequence;
using testing::StrEq;


class LocalIsolationModule : public IsolationModule
{
public:
  Executor* executor;
  MesosExecutorDriver* driver;
  string pid;

  LocalIsolationModule(Executor* _executor)
    : executor(_executor), driver(NULL) {}

  virtual ~LocalIsolationModule() {}

  virtual void initialize(Slave* slave) {
    pid = slave->self();
  }

  virtual void startExecutor(Framework* framework) {
    // TODO(benh): Cleanup the way we launch local drivers!
    setenv("MESOS_LOCAL", "1", 1);
    setenv("MESOS_SLAVE_PID", pid.c_str(), 1);
    setenv("MESOS_FRAMEWORK_ID", framework->frameworkId.value().c_str(), 1);

    driver = new MesosExecutorDriver(executor);
    driver->start();
  }

  virtual void killExecutor(Framework* framework) {
    driver->stop();
    driver->join();
    delete driver;

    // TODO(benh): Cleanup the way we launch local drivers!
    unsetenv("MESOS_LOCAL");
    unsetenv("MESOS_SLAVE_PID");
    unsetenv("MESOS_FRAMEWORK_ID");
  }
};


TEST(MasterTest, ResourceOfferWithMultipleSlaves)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<2>(&offers), Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());
  EXPECT_GE(10, offers.size());

  Resources resources(offers[0].resources());
  EXPECT_EQ(2, resources.getScalar("cpus", Resource::Scalar()).value());
  EXPECT_EQ(1024, resources.getScalar("mem", Resource::Scalar()).value());

  driver.stop();
  driver.join();

  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterReject)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(10, 2, 1 * Gigabyte, false, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  OfferID offerId;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), Trigger(&sched1ResourceOfferCall)));

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  driver1.replyToOffer(offerId, vector<TaskDescription>());

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall));

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOfferCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}


TEST(MasterTest, ResourcesReofferedAfterBadResponse)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, ElementsAre(_)))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&sched1ResourceOfferCall)));

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Resource::SCALAR);
  cpus->mutable_scalar()->set_value(0);

  Resource* mem = task.add_resources();
  mem->set_name("mem");
  mem->set_type(Resource::SCALAR);
  mem->mutable_scalar()->set_value(1 * Gigabyte);

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  trigger sched1ErrorCall;

  EXPECT_CALL(sched1,
              error(&driver1, _, "Invalid resources for task"))
    .WillOnce(Trigger(&sched1ErrorCall));

  EXPECT_CALL(sched1, offerRescinded(&driver1, offerId))
    .Times(AtMost(1));

  driver1.replyToOffer(offerId, tasks);

  WAIT_UNTIL(sched1ErrorCall);

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall));

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOfferCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}


TEST(MasterTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  ProcessBasedIsolationModule isolationModule;
  
  Slave s(resources, true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger resourceOfferCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  trigger offerRescindedCall, slaveLostCall;

  EXPECT_CALL(sched, offerRescinded(&driver, offerId))
    .WillOnce(Trigger(&offerRescindedCall));

  EXPECT_CALL(sched, slaveLost(&driver, offers[0].slave_id()))
    .WillOnce(Trigger(&slaveLostCall));

  MesosProcess::post(slave, S2S_SHUTDOWN);

  WAIT_UNTIL(offerRescindedCall);
  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  Process::wait(slave);

  MesosProcess::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}


TEST(MasterTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  FrameworkID frameworkId;

  trigger sched1RegisteredCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&frameworkId), Trigger(&sched1RegisteredCall)));

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1RegisteredCall);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master, frameworkId);

  trigger sched2RegisteredCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, resourceOffer(&driver2, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  local::shutdown();
}


TEST(MasterTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  MockFilter filter;
  Process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  PID master = local::launch(1, 2, 1 * Gigabyte, false, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  trigger slaveLostCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(Trigger(&slaveLostCall));

  EXPECT_MSG(filter, Eq(SH2M_HEARTBEAT), _, _)
    .WillRepeatedly(Return(true));

  driver.start();

  Clock::advance(master::HEARTBEAT_TIMEOUT);

  WAIT_UNTIL(slaveLostCall);

  driver.stop();
  driver.join();

  local::shutdown();

  Process::filter(NULL);

  Clock::resume();
}


TEST(MasterTest, TaskRunning)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  MockExecutor exec;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Slave s(resources, true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger resourceOfferCall, statusUpdateCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.stop();
  driver.join();

  MesosProcess::post(slave, S2S_SHUTDOWN);
  Process::wait(slave);

  MesosProcess::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}


TEST(MasterTest, KillTask)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  MockExecutor exec;

  trigger killTaskCall;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(Trigger(&killTaskCall));

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Slave s(resources, true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger resourceOfferCall, statusUpdateCall;

  EXPECT_CALL(sched, getFrameworkName(&driver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&driver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&driver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  driver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskID taskId;
  taskId.set_value("1");

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  driver.killTask(taskId);

  WAIT_UNTIL(killTaskCall);

  driver.stop();
  driver.join();

  MesosProcess::post(slave, S2S_SHUTDOWN);
  Process::wait(slave);

  MesosProcess::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}


TEST(MasterTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  MockFilter filter;
  Process::filter(&filter);

  EXPECT_MSG(filter, _, _, _)
    .WillRepeatedly(Return(false));

  Master m;
  PID master = Process::spawn(&m);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  MockExecutor exec;

  EXPECT_CALL(exec, init(_, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Slave s(resources, true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  FrameworkID frameworkId;
  OfferID offerId;
  vector<SlaveOffer> offers;

  trigger resourceOfferCall, statusUpdateMsg;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .Times(0);

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  EXPECT_MSG(filter, Eq(M2F_STATUS_UPDATE), _, Ne(master))
    .WillOnce(DoAll(Trigger(&statusUpdateMsg), Return(true)))
    .RetiresOnSaturation();

  driver1.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver1.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateMsg);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers, at which point advance time enough for the reliable
  // timeout to kick in and another status update message is sent.

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master, frameworkId);

  trigger registeredCall, statusUpdateCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&registeredCall));

  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(Trigger(&statusUpdateCall));

  driver2.start();

  WAIT_UNTIL(registeredCall);

  Clock::advance(RELIABLE_TIMEOUT);

  WAIT_UNTIL(statusUpdateCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  MesosProcess::post(slave, S2S_SHUTDOWN);
  Process::wait(slave);

  MesosProcess::post(master, M2M_SHUTDOWN);
  Process::wait(master);

  Process::filter(NULL);

  Clock::resume();
}


TEST(MasterTest, FrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  MockExecutor exec;

  ExecutorDriver* execDriver;
  ExecutorArgs args;
  FrameworkMessage execMessage;

  trigger execFrameworkMessageCall;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver), SaveArg<1>(&args)));

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(DoAll(SaveArg<1>(&execMessage),
                    Trigger(&execFrameworkMessageCall)));

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Slave s(resources, true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler and wait until the
  // first status update message is sent to it (drop the message).

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(&sched, master);

  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;
  FrameworkMessage schedMessage;

  trigger resourceOfferCall, statusUpdateCall, schedFrameworkMessageCall;

  EXPECT_CALL(sched, getFrameworkName(&schedDriver))
    .WillOnce(Return(""));

  EXPECT_CALL(sched, getExecutorInfo(&schedDriver))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched, registered(&schedDriver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffer(&schedDriver, _, _))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&resourceOfferCall)));

  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&statusUpdateCall)));

  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _))
    .WillOnce(DoAll(SaveArg<1>(&schedMessage),
                    Trigger(&schedFrameworkMessageCall)));

  schedDriver.start();

  WAIT_UNTIL(resourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  schedDriver.replyToOffer(offerId, tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  FrameworkMessage hello;
  *hello.mutable_slave_id() = offers[0].slave_id();
  hello.set_data("hello");

  schedDriver.sendFrameworkMessage(hello);

  WAIT_UNTIL(execFrameworkMessageCall);

  EXPECT_EQ("hello", execMessage.data());

  FrameworkMessage reply;
  *reply.mutable_slave_id() = args.slave_id();
  reply.set_data("reply");

  execDriver->sendFrameworkMessage(reply);

  WAIT_UNTIL(schedFrameworkMessageCall);

  EXPECT_EQ("reply", schedMessage.data());

  schedDriver.stop();
  schedDriver.join();

  MesosProcess::post(slave, S2S_SHUTDOWN);
  Process::wait(slave);

  MesosProcess::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}


TEST(MasterTest, SchedulerFailoverFrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Master m;
  PID master = Process::spawn(&m);

  Resources resources;

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Resource::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Resource::SCALAR);
  mem.mutable_scalar()->set_value(1 * Gigabyte);

  resources += cpus;
  resources += mem;

  MockExecutor exec;

  ExecutorDriver* execDriver;

  EXPECT_CALL(exec, init(_, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(1);

  EXPECT_CALL(exec, shutdown(_))
    .Times(1);

  LocalIsolationModule isolationModule(&exec);

  Slave s(resources, true, &isolationModule);
  PID slave = Process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, master);

  FrameworkID frameworkId;
  OfferID offerId;
  vector<SlaveOffer> offers;
  TaskStatus status;

  trigger sched1ResourceOfferCall, sched1StatusUpdateCall;

  EXPECT_CALL(sched1, getFrameworkName(&driver1))
    .WillOnce(Return(""));

  EXPECT_CALL(sched1, getExecutorInfo(&driver1))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched1, registered(&driver1, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&status), Trigger(&sched1StatusUpdateCall)));

  EXPECT_CALL(sched1, resourceOffer(&driver1, _, ElementsAre(_)))
    .WillOnce(DoAll(SaveArg<1>(&offerId), SaveArg<2>(&offers),
                    Trigger(&sched1ResourceOfferCall)));

  EXPECT_CALL(sched1, error(&driver1, _, "Framework failover"))
    .Times(1);

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  driver1.replyToOffer(offerId, tasks);

  WAIT_UNTIL(sched1StatusUpdateCall);

  EXPECT_EQ(TASK_RUNNING, status.state());

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, master, frameworkId);

  trigger sched2RegisteredCall, sched2FrameworkMessageCall;

  EXPECT_CALL(sched2, getFrameworkName(&driver2))
    .WillOnce(Return(""));

  EXPECT_CALL(sched2, getExecutorInfo(&driver2))
    .WillOnce(Return(DEFAULT_EXECUTOR_INFO));

  EXPECT_CALL(sched2, registered(&driver2, frameworkId))
    .WillOnce(Trigger(&sched2RegisteredCall));

  EXPECT_CALL(sched2, frameworkMessage(&driver2, _))
    .WillOnce(Trigger(&sched2FrameworkMessageCall));

  driver2.start();

  WAIT_UNTIL(sched2RegisteredCall);

  FrameworkMessage message;
  message.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  execDriver->sendFrameworkMessage(message);

  WAIT_UNTIL(sched2FrameworkMessageCall);

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  MesosProcess::post(slave, S2S_SHUTDOWN);
  Process::wait(slave);

  MesosProcess::post(master, M2M_SHUTDOWN);
  Process::wait(master);
}
