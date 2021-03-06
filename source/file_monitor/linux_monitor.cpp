#include "linux_monitor.hpp"
#include <boost/filesystem.hpp>
#include <sys/poll.h>

using namespace file_monitor;
namespace fs = boost::filesystem;

linux_monitor::linux_monitor()
{
  m_inotify_instance = inotify_init1(IN_NONBLOCK);
  if (m_inotify_instance == -1)
    throw std::runtime_error("Could not aquire an inotify instance!");
}

linux_monitor::~linux_monitor()
{
  stop();
  close(m_inotify_instance);
}

void linux_monitor::stop()
{
  assert(m_inotify_instance != -1);

  for (auto const& watch : m_watches)
    inotify_rm_watch(m_inotify_instance, watch.id);

  m_watches.clear();
  m_base_path = path_t();
  m_base_watch_id = -1;
}

void linux_monitor::create_watches(const path_t& root)
{
  using fs_rdi = fs::recursive_directory_iterator;

  auto id = inotify_add_watch(m_inotify_instance, root.c_str(), IN_ALL_EVENTS);
  if (id == -1)
    throw std::runtime_error("Initializing a watch on " + root.string());

  m_watches.emplace_back(watch_t{ id, root });

  for (auto i = fs_rdi(root); i != fs_rdi(); ++i)
  {
    if (!fs::is_directory(*i))
      continue;

    auto id = inotify_add_watch(m_inotify_instance, i->path().c_str(), IN_ALL_EVENTS);
    if (id == -1)
    {
      // TODO: do we want to handle this some other way here or is
      //       ignoring the subtree acceptable?
      i.pop();
      continue;
    }

    m_watches.emplace_back(watch_t{ id, *i });
  }
}

void linux_monitor::start(path_t const& base_path)
{
  // for now, we have to only monitor directories and their contents
  // since at least one other implementation has no file-level monitoring
  // support.
  if (!fs::is_directory(base_path))
    throw std::runtime_error("Base path does not exist or is not a directory!");

  m_base_path = base_path;
  create_watches(m_base_path);
  assert(!m_watches.empty());
  m_base_watch_id = m_watches.front().id;
}

void linux_monitor::poll(monitor::change_event_t const& consumer)
{
  pollfd poll_descriptor = { m_inotify_instance, POLLIN };
  ::poll(&poll_descriptor, 1, 0);

  // check whether there are actual events to process
  if (!(poll_descriptor.revents & POLLIN))
    return;

  process_events(consumer);
}

void linux_monitor::process_events(monitor::change_event_t const& consumer)
{
  std::array<char, 4096> events;
  std::vector<path_t> changed_files;

  for (;;)
  {
    auto bytes_read = read(m_inotify_instance, events.data(), events.size());

    if (bytes_read <= 0)
    {
      consumer(changed_files);
      break;
    }

    inotify_event const* event = nullptr;
    for (auto i = events.data(); i < events.data() + bytes_read;
         i += sizeof(inotify_event) + event->len)
    {
      event = reinterpret_cast<inotify_event const*>(i);

      auto const& watch = find_watch(event->wd);
      if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF) && watch.id == m_base_watch_id)
      {
        stop();
        continue;
      }

      // TODO: investigate other cases other than IN_DELETE_SELF where
      //       event->len can equal 0.
      if (event->len == 0)
        continue;

      auto const changed_path = watch.base_path / event->name;

      // different events consitute a file change. some tools
      // create temporary files and then move them to the actual
      // file being watched others simplc y do a normal open/modify/
      // close cylce. file modifications are basically all events
      // that either move TO the file or close the file after writing.
      if (event->mask & IN_CLOSE_WRITE || event->mask & IN_MOVED_TO)
      {
        auto i = std::find(changed_files.begin(), changed_files.end(), changed_path);
        if (i == changed_files.end())
          changed_files.push_back(std::move(fs::relative(changed_path, m_base_path)));
      }

      auto const absolute_path = fs::absolute(changed_path, m_base_path);
      if (event->mask & IN_CREATE && fs::is_directory(absolute_path))
      {
        create_watches(absolute_path);
      }
      else if (event->mask & (IN_DELETE | IN_MOVED_FROM))
      {
        // we cannot filter here using fs::is_directory() anymore
        // since the entity has already been deleted. we need to
        // find the stale watch and remove
        auto watch = std::find_if(
          m_watches.cbegin(), m_watches.cend(), [absolute_path, this](auto const& watch) {
            auto watch_path_abs = fs::absolute(watch.base_path, this->m_base_path);
            return watch_path_abs == absolute_path.lexically_normal();
          });

        if (watch != m_watches.end())
        {
          inotify_rm_watch(m_inotify_instance, watch->id);
          m_watches.erase(watch);
        }
      }

      // TODO: find out whether watching for IN_MODIFY is necessary
      // TODO: we don't care for IN_OPEN, IN_ACCESS or IN_ATTRIBS and IN_CLOSE_NOWRITE
      //       events here, at least not yet

      // NOTE: Renaming will set the "cookie" ID of the IN_MOVED_FROM/IN_MOVED_TO pair,
      //       but we may not even see one of the events. For now, we ignore the cookie.
    }
  }
}

linux_monitor::watch_t const& linux_monitor::find_watch(int id) const
{
  auto watch = std::find_if(
    m_watches.cbegin(), m_watches.cend(), [id](auto const& watch) { return watch.id == id; });
  assert(watch != m_watches.cend());
  return *watch;
}

path_t linux_monitor::base_path() const
{
  return m_base_path;
}
