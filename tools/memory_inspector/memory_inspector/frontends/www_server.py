# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""This module implements a simple WSGI server for the memory_inspector Web UI.

The WSGI server essentially handles two kinds of requests:
 - /ajax/foo/bar: The AJAX endpoints which exchange JSON data with the JS.
    Requests routing is achieved using a simple @uri decorator which simply
    performs regex matching on the request path.
 - /static/content: Anything not matching the /ajax/ prefix is treated as a
    static content request (for serving the index.html and JS/CSS resources).

The following HTTP status code are returned by the server:
 - 200 - OK: The request was handled correctly.
 - 404 - Not found: None of the defined handlers did match the /request/path.
 - 410 - Gone: The path was matched but the handler returned an empty response.
    This typically happens when the target device is disconnected.
"""

import collections
import datetime
import os
import memory_inspector
import mimetypes
import json
import re
import urlparse
import uuid
import wsgiref.simple_server

from memory_inspector.core import backends
from memory_inspector.classification import mmap_classifier
from memory_inspector.data import serialization
from memory_inspector.data import file_storage


_HTTP_OK = '200 - OK'
_HTTP_GONE = '410 - Gone'
_HTTP_NOT_FOUND = '404 - Not Found'
_PERSISTENT_STORAGE_PATH = os.path.join(
    os.path.expanduser('~'), '.config', 'memory_inspector')
_CONTENT_DIR = os.path.abspath(os.path.join(
    os.path.dirname(__file__), 'www_content'))
_APP_PROCESS_RE = r'^[\w.:]+$'  # Regex for matching app processes.
_STATS_HIST_SIZE = 120  # Keep at most 120 samples of stats per process.
_CACHE_LEN = 10  # Max length of |_cached_objs|.

# |_cached_objs| keeps the state of short-lived objects that the client needs to
# _cached_objs subsequent AJAX calls.
_cached_objs = collections.OrderedDict()
_persistent_storage = file_storage.Storage(_PERSISTENT_STORAGE_PATH)
_proc_stats_history = {}  # /Android/device/PID -> deque([stats@T=0, stats@T=1])


class UriHandler(object):
  """Base decorator used to automatically route /requests/by/path.

  Each handler is called with the following args:
    args: a tuple of the matching regex groups.
    req_vars: a dictionary of request args (querystring for GET, body for POST).
  Each handler must return a tuple with the following elements:
    http_code: a string with the HTTP status code (e.g., '200 - OK')
    headers: a list of HTTP headers (e.g., [('Content-Type': 'foo/bar')])
    body: the HTTP response body.
  """
  _handlers = []

  def __init__(self, path_regex, verb='GET', output_filter=None):
    self._path_regex = path_regex
    self._verb = verb
    default_output_filter = lambda *x: x  # Just return the same args unchanged.
    self._output_filter = output_filter or default_output_filter

  def __call__(self, handler):
    UriHandler._handlers += [(
        self._verb, self._path_regex, self._output_filter, handler)]

  @staticmethod
  def Handle(method, path, req_vars):
    """Finds a matching handler and calls it (or returns a 404 - Not Found)."""
    for (match_method, path_regex, output_filter, fn) in UriHandler._handlers:
      if method != match_method:
        continue
      m = re.match(path_regex, path)
      if not m:
        continue
      (http_code, headers, body) = fn(m.groups(), req_vars)
      return output_filter(http_code, headers, body)
    return (_HTTP_NOT_FOUND, [], 'No AJAX handlers found')


class AjaxHandler(UriHandler):
  """Decorator for routing AJAX requests.

  This decorator essentially groups the JSON serialization and the cache headers
  which is shared by most of the handlers defined below.
  """
  def __init__(self, path_regex, verb='GET'):
    super(AjaxHandler, self).__init__(
        path_regex, verb, AjaxHandler.AjaxOutputFilter)

  @staticmethod
  def AjaxOutputFilter(http_code, headers, body):
    serialized_content = json.dumps(body, cls=serialization.Encoder)
    extra_headers = [('Cache-Control', 'no-cache'),
                     ('Expires', 'Fri, 19 Sep 1986 05:00:00 GMT')]
    return http_code, headers + extra_headers, serialized_content


@AjaxHandler('/ajax/backends')
def _ListBackends(args, req_vars):  # pylint: disable=W0613
  return _HTTP_OK, [], [backend.name for backend in backends.ListBackends()]


@AjaxHandler('/ajax/devices')
def _ListDevices(args, req_vars):  # pylint: disable=W0613
  resp = []
  for device in backends.ListDevices():
    # The device settings must loaded at discovery time (i.e. here), not during
    # startup, because it might have been plugged later.
    for k, v in _persistent_storage.LoadSettings(device.id).iteritems():
      device.settings[k] = v

    resp += [{'backend': device.backend.name,
              'id': device.id,
              'name': device.name}]
  return _HTTP_OK, [], resp


@AjaxHandler(r'/ajax/dump/mmap/(\w+)/(\w+)/(\d+)')
def _DumpMmapsForProcess(args, req_vars):  # pylint: disable=W0613
  """Dumps memory maps for a process.

  The response is formatted according to the Google Charts DataTable format.
  """
  process = _GetProcess(args)
  if not process:
    return _HTTP_GONE, [], 'Device not found or process died'
  mmap = process.DumpMemoryMaps()
  table = {
      'cols': [
          {'label': 'Start', 'type':'string'},
          {'label': 'End', 'type':'string'},
          {'label': 'Length Kb', 'type':'number'},
          {'label': 'Prot', 'type':'string'},
          {'label': 'Priv. Dirty Kb', 'type':'number'},
          {'label': 'Priv. Clean Kb', 'type':'number'},
          {'label': 'Shared Dirty Kb', 'type':'number'},
          {'label': 'Shared Clean Kb', 'type':'number'},
          {'label': 'File', 'type':'string'},
          {'label': 'Offset', 'type':'number'},
          {'label': 'Resident Pages', 'type':'string'},
        ],
      'rows': []}
  for entry in mmap.entries:
    table['rows'] += [{'c': [
        {'v': '%08x' % entry.start, 'f': None},
        {'v': '%08x' % entry.end, 'f': None},
        {'v': entry.len / 1024, 'f': None},
        {'v': entry.prot_flags, 'f': None},
        {'v': entry.priv_dirty_bytes / 1024, 'f': None},
        {'v': entry.priv_clean_bytes / 1024, 'f': None},
        {'v': entry.shared_dirty_bytes / 1024, 'f': None},
        {'v': entry.shared_clean_bytes / 1024, 'f': None},
        {'v': entry.mapped_file, 'f': None},
        {'v': entry.mapped_offset, 'f': None},
        {'v': '[%s]' % (','.join(map(str, entry.resident_pages))), 'f': None},
    ]}]
  # Store the dump in the cache. The client might need it later for profiling.
  cache_id = _CacheObject(mmap)
  return _HTTP_OK, [], {'table': table, 'id': cache_id}


@AjaxHandler('/ajax/initialize/(\w+)/(\w+)$')  # /ajax/initialize/Android/a0b1c2
def _InitializeDevice(args, req_vars):  # pylint: disable=W0613
  device = _GetDevice(args)
  if not device:
    return _HTTP_GONE, [], 'Device not found'
  device.Initialize()
  return _HTTP_OK, [], {
    'isNativeTracingEnabled': device.IsNativeTracingEnabled()}


@AjaxHandler(r'/ajax/profile/create', 'POST')
def _CreateProfile(args, req_vars):  # pylint: disable=W0613
  """Creates (and caches) a profile from a set of dumps.

  The profiling data can be retrieved afterwards using the /profile/{PROFILE_ID}
  endpoints (below).
  """
  classifier = None  # A classifier module (/classification/*_classifier.py).
  dumps = {}  # dump-time -> obj. to classify (e.g., |memory_map.Map|).
  for arg in 'type', 'source', 'ruleset', 'id':
    assert(arg in req_vars), 'Expecting %s argument in POST data' % arg

  # Step 1: collect the memory dumps, according to what the client specified in
  # the 'type' and 'source' POST arguments.

  # Case 1: Generate a profile from a set of mmap dumps.
  if req_vars['type'] == 'mmap':
    classifier = mmap_classifier
    # Case 1a: Use a cached mmap dumps.
    if req_vars['source'] == 'cache':
      dumps[0] = _GetCacheObject(req_vars['id'])
    # TODO(primiano): add support for loading archived dumps from file_storage.
  # TODO(primiano): Add support for native_heap types.

  # Step 2: Load the rule-set specified by the client in the 'ruleset' POST arg.
  # Also, perform some basic sanity checking.
  rules_path = os.path.join(memory_inspector.ROOT_DIR, 'classification_rules',
                            req_vars['ruleset'])
  if not classifier:
    return _HTTP_GONE, [], 'Classifier %s not supported.' % req_vars['type']
  if not dumps:
    return _HTTP_GONE, [], 'No memory dumps could be retrieved'
  if not os.path.isfile(rules_path):
    return _HTTP_GONE, [], 'Cannot find the rule-set %s' % rules_path
  with open(rules_path) as f:
    rules = mmap_classifier.LoadRules(f.read())

  # Step 3: Aggregate the data using the desired classifier and generate the
  # profile dictionary (which will be kept cached here in the server).
  # The resulting profile will consist of 1+ snapshots (depending on the number
  # dumps the client has requested to process) and a number of 1+ metrics
  # (depending on the buckets' keys returned by the classifier).

  # Converts the {time: dump_obj} dict into a {time: |AggregatedResult|} dict.
  # using the classifier.
  snapshots = collections.OrderedDict(
    (time, classifier.Classify(d, rules)) for time, d in dumps.iteritems())

  # Add the profile to the cache (and eventually discard old items).
  # |profile_id| is the key that the client will use in subsequent requests
  # (to the /ajax/profile/{ID}/ endpoints) to refer to this particular profile.
  profile_id = _CacheObject(snapshots)

  first_snapshot = next(snapshots.itervalues())

  # |metrics| is the key set of any of the aggregated result
  return _HTTP_OK, [], {'id': profile_id,
                        'times': snapshots.keys(),
                        'metrics': first_snapshot.keys,
                        'rootBucket': first_snapshot.total.name + '/'}


@AjaxHandler(r'/ajax/profile/(\w+)/tree/(\d+)/(\d+)')
def _GetProfileTreeDataForSnapshot(args, req_vars):  # pylint: disable=W0613
  """Gets the data for the tree chart for a given time and metric.

  The response is formatted according to the Google Charts DataTable format.
  """
  snapshot_id = args[0]
  metric_index = int(args[1])
  time = int(args[2])
  snapshots = _GetCacheObject(snapshot_id)
  if not snapshots:
    return _HTTP_GONE, [], 'Cannot find the selected profile.'
  if time not in snapshots:
    return _HTTP_GONE, [], 'Cannot find snapshot at T=%d.' % time
  snapshot = snapshots[time]
  if metric_index >= len(snapshot.keys):
    return _HTTP_GONE, [], 'Invalid metric id %d' % metric_index

  resp = {'cols': [{'label': 'bucket', 'type': 'string'},
                   {'label': 'parent', 'type': 'string'}],
          'rows': []}

  def VisitBucketAndAddRows(bucket, parent_id=''):
    """Recursively creates the (node, parent) visiting |ResultTree| in DFS."""
    node_id = parent_id + bucket.name + '/'
    node_label = '<dl><dt>%s</dt><dd>%s</dd></dl>' % (
        bucket.name, _StrMem(bucket.values[metric_index]))
    resp['rows'] += [{'c': [
        {'v': node_id, 'f': node_label},
        {'v': parent_id, 'f': None},
    ]}]
    for child in bucket.children:
      VisitBucketAndAddRows(child, node_id)

  VisitBucketAndAddRows(snapshot.total)
  return _HTTP_OK, [], resp


@AjaxHandler(r'/ajax/profile/(\w+)/time_serie/(\d+)/(.*)$')
def _GetTimeSerieForSnapshot(args, req_vars):  # pylint: disable=W0613
  """Gets the data for the area chart for a given metric and bucket.

  The response is formatted according to the Google Charts DataTable format.
  """
  snapshot_id = args[0]
  metric_index = int(args[1])
  bucket_path = args[2]
  snapshots = _GetCacheObject(snapshot_id)
  if not snapshots:
    return _HTTP_GONE, [], 'Cannot find the selected profile.'
  if metric_index >= len(next(snapshots.itervalues()).keys):
    return _HTTP_GONE, [], 'Invalid metric id %d' % metric_index

  def FindBucketByPath(bucket, path, parent_path=''):  # Essentially a DFS.
    cur_path = parent_path + bucket.name + '/'
    if cur_path == path:
      return bucket
    for child in bucket.children:
      res = FindBucketByPath(child, path, cur_path)
      if res:
        return res
    return None

  # The resulting data table will look like this (assuming len(metrics) == 2):
  # Time  Ashmem      Dalvik     Other
  # 0    (1024,0)  (4096,1024)  (0,0)
  # 30   (512,512) (1024,1024)  (0,512)
  # 60   (0,512)   (1024,0)     (512,0)
  resp = {'cols': [], 'rows': []}
  for time, aggregated_result in snapshots.iteritems():
    bucket = FindBucketByPath(aggregated_result.total, bucket_path)
    if not bucket:
      return _HTTP_GONE, [], 'Bucket %s not found' % bucket_path

    # If the user selected a non-leaf bucket, display the breakdown of its
    # direct children. Otherwise just the leaf bucket.
    children_buckets = bucket.children if bucket.children else [bucket]

    # Create the columns (form the buckets) when processing the first snapshot.
    if not resp['cols']:
      resp['cols'] += [{'label': 'Time', 'type': 'string'}]
      for child_bucket in children_buckets:
        resp['cols'] += [{'label': child_bucket.name, 'type': 'number'}]

    row = [{'v': str(time), 'f': None}]
    for child_bucket in children_buckets:
      row += [{'v': child_bucket.values[metric_index] / 1024, 'f': None}]
    resp['rows'] += [{'c': row}]

  return _HTTP_OK, [], resp


@AjaxHandler(r'/ajax/ps/(\w+)/(\w+)$')  # /ajax/ps/Android/a0b1c2[?all=1]
def _ListProcesses(args, req_vars):  # pylint: disable=W0613
  """Lists processes and their CPU / mem stats.

  The response is formatted according to the Google Charts DataTable format.
  """
  device = _GetDevice(args)
  if not device:
    return _HTTP_GONE, [], 'Device not found'
  resp = {
      'cols': [
          {'label': 'Pid', 'type':'number'},
          {'label': 'Name', 'type':'string'},
          {'label': 'Cpu %', 'type':'number'},
          {'label': 'Mem RSS Kb', 'type':'number'},
          {'label': '# Threads', 'type':'number'},
        ],
      'rows': []}
  for process in device.ListProcesses():
    # Exclude system apps if the request didn't contain the ?all=1 arg.
    if not req_vars.get('all') and not re.match(_APP_PROCESS_RE, process.name):
      continue
    stats = process.GetStats()
    resp['rows'] += [{'c': [
        {'v': process.pid, 'f': None},
        {'v': process.name, 'f': None},
        {'v': stats.cpu_usage, 'f': None},
        {'v': stats.vm_rss, 'f': None},
        {'v': stats.threads, 'f': None},
    ]}]
  return _HTTP_OK, [], resp


@AjaxHandler(r'/ajax/stats/(\w+)/(\w+)$')  # /ajax/stats/Android/a0b1c2
def _GetDeviceStats(args, req_vars):  # pylint: disable=W0613
  """Lists device CPU / mem stats.

  The response is formatted according to the Google Charts DataTable format.
  """
  device = _GetDevice(args)
  if not device:
    return _HTTP_GONE, [], 'Device not found'
  device_stats = device.GetStats()

  cpu_stats = {
      'cols': [
          {'label': 'CPU', 'type':'string'},
          {'label': 'Usr %', 'type':'number'},
          {'label': 'Sys %', 'type':'number'},
          {'label': 'Idle %', 'type':'number'},
        ],
      'rows': []}

  for cpu_idx in xrange(len(device_stats.cpu_times)):
    cpu = device_stats.cpu_times[cpu_idx]
    cpu_stats['rows'] += [{'c': [
        {'v': '# %d' % cpu_idx, 'f': None},
        {'v': cpu['usr'], 'f': None},
        {'v': cpu['sys'], 'f': None},
        {'v': cpu['idle'], 'f': None},
    ]}]

  mem_stats = {
      'cols': [
          {'label': 'Section', 'type':'string'},
          {'label': 'MB', 'type':'number',  'pattern': ''},
        ],
      'rows': []}

  for key, value in device_stats.memory_stats.iteritems():
    mem_stats['rows'] += [{'c': [
        {'v': key, 'f': None},
        {'v': value / 1024, 'f': None}
    ]}]

  return _HTTP_OK, [], {'cpu': cpu_stats, 'mem': mem_stats}


@AjaxHandler(r'/ajax/stats/(\w+)/(\w+)/(\d+)$')  # /ajax/stats/Android/a0b1c2/42
def _GetProcessStats(args, req_vars):  # pylint: disable=W0613
  """Lists CPU / mem stats for a given process (and keeps history).

  The response is formatted according to the Google Charts DataTable format.
  """
  process = _GetProcess(args)
  if not process:
    return _HTTP_GONE, [], 'Device not found'

  proc_uri = '/'.join(args)
  cur_stats = process.GetStats()
  if proc_uri not in _proc_stats_history:
    _proc_stats_history[proc_uri] = collections.deque(maxlen=_STATS_HIST_SIZE)
  history = _proc_stats_history[proc_uri]
  history.append(cur_stats)

  cpu_stats = {
      'cols': [
          {'label': 'T', 'type':'string'},
          {'label': 'CPU %', 'type':'number'},
          {'label': '# Threads', 'type':'number'},
        ],
      'rows': []
  }

  mem_stats = {
      'cols': [
          {'label': 'T', 'type':'string'},
          {'label': 'Mem RSS Kb', 'type':'number'},
          {'label': 'Page faults', 'type':'number'},
        ],
      'rows': []
  }

  for stats in history:
    cpu_stats['rows'] += [{'c': [
          {'v': str(datetime.timedelta(seconds=stats.run_time)), 'f': None},
          {'v': stats.cpu_usage, 'f': None},
          {'v': stats.threads, 'f': None},
    ]}]
    mem_stats['rows'] += [{'c': [
          {'v': str(datetime.timedelta(seconds=stats.run_time)), 'f': None},
          {'v': stats.vm_rss, 'f': None},
          {'v': stats.page_faults, 'f': None},
    ]}]

  return _HTTP_OK, [], {'cpu': cpu_stats, 'mem': mem_stats}


@AjaxHandler(r'/ajax/settings/(\w+)/?(\w+)?$')  # /ajax/settings/Android[/id]
def _GetDeviceOrBackendSettings(args, req_vars):  # pylint: disable=W0613
  backend = backends.GetBackend(args[0])
  if not backend:
    return _HTTP_GONE, [], 'Backend not found'
  if args[1]:
    device = _GetDevice(args)
    if not device:
      return _HTTP_GONE, [], 'Device not found'
    settings = device.settings
  else:
    settings = backend.settings

  assert(isinstance(settings, backends.Settings))
  resp = {}
  for key  in settings.expected_keys:
    resp[key] = {'description': settings.expected_keys[key],
                 'value': settings.values[key]}
  return _HTTP_OK, [], resp


@AjaxHandler(r'/ajax/settings/(\w+)/?(\w+)?$', 'POST')
def _SetDeviceOrBackendSettings(args, req_vars):  # pylint: disable=W0613
  backend = backends.GetBackend(args[0])
  if not backend:
    return _HTTP_GONE, [], 'Backend not found'
  if args[1]:
    device = _GetDevice(args)
    if not device:
      return _HTTP_GONE, [], 'Device not found'
    settings = device.settings
    storage_name = device.id
  else:
    settings = backend.settings
    storage_name = backend.name

  for key in req_vars.iterkeys():
    settings[key] = req_vars[key]
  _persistent_storage.StoreSettings(storage_name, settings.values)
  return _HTTP_OK, [], ''


@UriHandler(r'^(?!/ajax)/(.*)$')
def _StaticContent(args, req_vars):  # pylint: disable=W0613
  # Give the browser a 1-day TTL cache to minimize the start-up time.
  cache_headers = [('Cache-Control', 'max-age=86400, public')]
  req_path = args[0] if args[0] else 'index.html'
  file_path = os.path.abspath(os.path.join(_CONTENT_DIR, req_path))
  if (os.path.isfile(file_path) and
      os.path.commonprefix([file_path, _CONTENT_DIR]) == _CONTENT_DIR):
    mtype = 'text/plain'
    guessed_mime = mimetypes.guess_type(file_path)
    if guessed_mime and guessed_mime[0]:
      mtype = guessed_mime[0]
    with open(file_path, 'rb') as f:
      body = f.read()
    return _HTTP_OK, cache_headers + [('Content-Type', mtype)], body
  return _HTTP_NOT_FOUND, cache_headers,  file_path + ' not found'


def _GetDevice(args):
  """Returns a |backends.Device| instance from a /backend/device URI."""
  assert(len(args) >= 2), 'Malformed request. Expecting /backend/device'
  return backends.GetDevice(backend_name=args[0], device_id=args[1])


def _GetProcess(args):
  """Returns a |backends.Process| instance from a /backend/device/pid URI."""
  assert(len(args) >= 3 and args[2].isdigit()), (
      'Malformed request. Expecting /backend/device/pid')
  device = _GetDevice(args)
  if not device:
    return None
  return device.GetProcess(int(args[2]))


def _CacheObject(obj_to_store):
  """Stores an object in the server-side cache and returns its unique id."""
  if len(_cached_objs) >= _CACHE_LEN:
    _cached_objs.popitem(last=False)
  obj_id = uuid.uuid4().hex
  _cached_objs[obj_id] = obj_to_store
  return str(obj_id)


def _GetCacheObject(obj_id):
  """Retrieves an object in the server-side cache by its id."""
  return _cached_objs.get(obj_id)


def _StrMem(nbytes):
  """Converts a number (of bytes) into a human readable string (kb, mb)."""
  if nbytes < 2**10:
    return '%d B' % nbytes
  if nbytes < 2**20:
    return '%.1f KB' % round(nbytes / 1024.0)
  return '%.1f MB' % (nbytes / 1048576.0)


def _HttpRequestHandler(environ, start_response):
  """Parses a single HTTP request and delegates the handling through UriHandler.

  This essentially wires up wsgiref.simple_server with our @UriHandler(s).
  """
  path = environ['PATH_INFO']
  method = environ['REQUEST_METHOD']
  if method == 'POST':
    req_body_size = int(environ.get('CONTENT_LENGTH', 0))
    req_body = environ['wsgi.input'].read(req_body_size)
    req_vars = json.loads(req_body)
  else:
    req_vars = urlparse.parse_qs(environ['QUERY_STRING'])
  (http_code, headers, body) = UriHandler.Handle(method, path, req_vars)
  start_response(http_code, headers)
  return [body]


def Start(http_port):
  # Load the saved backends' settings (some of them might be needed to bootstrap
  # as, for instance, the adb path for the Android backend).
  for backend in backends.ListBackends():
    for k, v in _persistent_storage.LoadSettings(backend.name).iteritems():
      backend.settings[k] = v

  httpd = wsgiref.simple_server.make_server('', http_port, _HttpRequestHandler)
  httpd.serve_forever()