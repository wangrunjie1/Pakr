export default {
  async fetch(request, env) {

    if (request.method === 'OPTIONS') return cors(new Response(null, { status: 204 }), env);
    const url = new URL(request.url);
    try {
      let res;
      if      (url.pathname === '/build'    && request.method === 'POST') res = await handleBuild(request, env);
      else if (url.pathname === '/status'   && request.method === 'GET')  res = await handleStatus(request, env);
      else if (url.pathname === '/logs'     && request.method === 'GET')  res = await handleLogs(request, env);
      else if (url.pathname === '/download' && request.method === 'GET')  res = await handleDownload(request, env);
      else if (url.pathname === '/cancel'   && request.method === 'POST') res = await handleCancel(request, env);
      else return env.ASSETS.fetch(request);
      return cors(res, env);
    } catch (e) {
      if (env.ASSETS) {
        try {
          const url = new URL(request.url);
          if (!['/build','/status','/logs','/download','/cancel'].includes(url.pathname)) return cors(await env.ASSETS.fetch(request), env);
        } catch {}
      }
      return cors(json({ error: e.message }, 500), env);
    }
  }
};


function isSafeUrl(u) {
  try {
    const parsed = new URL(u);
    if (!['http:', 'https:'].includes(parsed.protocol)) return false;
    const host = parsed.hostname.toLowerCase();
    if (host === 'localhost' || host === '0.0.0.0') return false;
    if (/^127\./.test(host) || /^10\./.test(host) || /^192\.168\./.test(host)) return false;
    if (/^172\.(1[6-9]|2\d|3[0-1])\./.test(host)) return false;
    if (host === '::1' || host.startsWith('fc') || host.startsWith('fd')) return false;
    return true;
  } catch { return false; }
}

async function handleBuild(request, env) {
  const input = await request.json();
  if (input?.platform === 'windows') return handleWindowsBuild(input, env);
  const { app_url, app_name, package_name, version_name, icon_url, pull_to_refresh } = input;
  const buildId = Date.now().toString(36) + Math.random().toString(36).slice(2,6);
  if (!app_url || !app_name || !package_name || !version_name)
    return json({ error: 'Missing required fields' }, 400);
  const pkgRe = /^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*){1,}$/;
  if (!pkgRe.test(package_name))
    return json({ error: 'Invalid package name' }, 400);
  if (!isSafeUrl(app_url))
    return json({ error: 'Invalid or unsafe app_url' }, 400);
  if (icon_url && !isSafeUrl(icon_url))
    return json({ error: 'Invalid or unsafe icon_url' }, 400);
  const allowPullToRefresh = pull_to_refresh === true || pull_to_refresh === 'true';
  // 校验包名各段不能是 Java 关键字
  const JAVA_KEYWORDS = new Set(["abstract","assert","boolean","break","byte","case","catch","char","class","const","continue","default","do","double","else","enum","extends","final","finally","float","for","goto","if","implements","import","instanceof","int","interface","long","native","new","package","private","protected","public","return","short","static","strictfp","super","switch","synchronized","this","throw","throws","transient","try","void","volatile","while"]);
  const invalidSeg = package_name.split('.').find(seg => JAVA_KEYWORDS.has(seg));
  if (invalidSeg)
    return json({ error: `Package segment '${invalidSeg}' is a Java keyword and cannot be used in a package name` }, 400);
  // version_name 支持任意字符（1.0、2.0-beta、v3.1.0-rc1 等），仅限制长度
  if (!version_name || version_name.length > 32)
    return json({ error: 'version_name must be 1-32 characters' }, 400);

  const r = await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/workflows/build.yml/dispatches`,
    { method: 'POST', body: JSON.stringify({
        ref: 'main',
        inputs: {
          app_url,
          app_name,
          package_name,
          version_name,
          icon_url: icon_url || '',
          pull_to_refresh: allowPullToRefresh ? 'true' : 'false',
        }
    })}
  );
  if (r.status !== 204) return json({ error: 'Trigger failed', detail: await r.text() }, 500);

  // 立即返回，避免在 Worker 请求链路中等待 run_id 导致前端卡住
  return json({ status: 'queued', build_id: buildId, dispatched_at: new Date().toISOString() });
}

const windowsWorkflow = 'build-windows.yml';

async function handleWindowsBuild({ app_url, app_name, package_name, version_name, icon_url = '' }, env) {
  if (typeof app_url !== 'string' || app_url.length > 2048 || !isSafeUrl(app_url)) {
    return json({ error: '请输入有效的网页地址' }, 400);
  }
  const target = new URL(app_url);
  if (target.protocol !== 'https:' || target.username || target.password) {
    return json({ error: 'Windows 打包仅支持 HTTPS 网页地址' }, 400);
  }
  if (typeof app_name !== 'string' || !/^[\p{L}\p{M}\p{N} _.-]{1,60}$/u.test(app_name) || !app_name.trim()) {
    return json({ error: '应用名称须为 1–60 个字，支持文字、数字、空格、下划线、连字符和点号' }, 400);
  }
  if (typeof package_name !== 'string' || !/^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*){1,}$/.test(package_name) || package_name.length > 100) {
    return json({ error: '应用标识格式有误，例如 com.example.myapp' }, 400);
  }
  if (typeof version_name !== 'string' || !/^\d{1,5}(\.\d{1,5}){2}$/.test(version_name)
      || version_name.split('.').some(part => Number(part) > 65535)) {
    return json({ error: '版本号需为三段数字，例如 1.0.0，每段不超过 65535' }, 400);
  }
  if (icon_url !== '' && (typeof icon_url !== 'string' || icon_url.length > 2048 || !isSafeUrl(icon_url)
      || new URL(icon_url).username || new URL(icon_url).password)) {
    return json({ error: '请输入有效的图标 URL' }, 400);
  }
  const buildId = crypto.randomUUID();
  const resp = await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/workflows/${windowsWorkflow}/dispatches`, {
      method: 'POST',
      body: JSON.stringify({ ref: 'main', inputs: { build_id: buildId, app_url: target.href, app_name: app_name.trim(), package_name, version_name, icon_url } }),
    });
  if (!resp.ok) return json({ error: `Windows 构建提交失败（GitHub ${resp.status}）` }, 502);
  const runId = resp.status === 200 ? (await resp.json()).workflow_run_id : null;
  return json({ status: 'queued', build_id: buildId, run_id: runId || null });
}

async function handleStatus(request, env) {
  const u = new URL(request.url);
  let runId = u.searchParams.get('run_id');
  const buildId = u.searchParams.get('build_id');

  if (!runId && buildId) {
    const windows = u.searchParams.get('platform') === 'windows';
    const workflow = windows ? windowsWorkflow : 'build.yml';
    const dispatchedAt = u.searchParams.get('dispatched_at');
    const runsRes = await gh(env,
      `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/workflows/${workflow}/runs?event=workflow_dispatch&per_page=20`
    );
    const runs = await runsRes.json();
    const baseTs = dispatchedAt ? Date.parse(dispatchedAt) : Date.now();
    const matched = (runs.workflow_runs || []).find(r => {
      if (windows) return r.display_title === `Pakr Windows ${buildId}`;
      const created = Date.parse(r.created_at || 0);
      return Number.isFinite(created) && created >= (baseTs - 15000);
    });
    if (!matched) return json({ status: 'queued', waiting_run_id: true, build_id: buildId, dispatched_at: dispatchedAt });
    runId = matched.id;
  }

  if (!runId) return json({ error: 'Missing run_id' }, 400);
  const data = await (await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/runs/${runId}`
  )).json();
  const result = { run_id: runId, build_id: buildId || null, status: data.status, conclusion: data.conclusion, job_id: null, step_index: 0, step_total: 0 };

  // 解析 job steps 获取精确进度
  const jobsRes = await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/runs/${runId}/jobs`
  );
  const jobs = await jobsRes.json();
  const job = jobs.jobs?.[0];
  if (job) {
    result.job_id = job.id;
    const steps = job.steps || [];
    const userSteps = steps.filter(s => !['Set up job','Post','Complete job','Cache'].some(k => s.name.includes(k)));
    const stepMap = { 'Inject':15,'Process':30,'Build':70,'Sign':90,'Upload':100 };
    let progress = 5, currentStep = '', stepIndex = 0;
    for (const step of userSteps) {
      if (step.status === 'completed' && step.conclusion === 'success') {
        stepIndex++;
        for (const [name, pct] of Object.entries(stepMap)) {
          if (step.name.includes(name)) progress = Math.max(progress, pct);
        }
      }
      if (step.status === 'in_progress') {
        currentStep = step.name;
        const base = Object.entries(stepMap).find(([n]) => step.name.includes(n));
        if (base) progress = Math.max(progress, base[1] - 10);
      }
    }
    result.progress    = progress;
    result.current_step = currentStep;
    result.step_index  = stepIndex;
    result.step_total  = userSteps.length || 5;
  }

  if (data.status === 'completed' && data.conclusion === 'success') {
    result.progress = 100;
    const arts = await (await gh(env,
      `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/runs/${runId}/artifacts`
    )).json();
    // 返回全部 artifacts，让前端展示多个 APK 下载选项
    if (arts.artifacts?.length) {
      result.artifacts = arts.artifacts.map(a => ({
        id: a.id,
        name: a.name,
      }));
      // 兼容旧字段
      result.artifact_id   = arts.artifacts[0].id;
      result.artifact_name = arts.artifacts[0].name;
    }
  }

  // 构建失败时找出失败步骤，并拉取该步骤的日志片段
  if (data.status === 'completed' && data.conclusion === 'failure') {
    const steps = jobs.jobs?.[0]?.steps || [];
    const failedStep = steps.find(s => s.conclusion === 'failure');
    if (failedStep) {
      result.failed_step = failedStep.name;
      // 拉取日志，截取最后 30 行作为错误摘要
      try {
        const logRes = await gh(env,
          `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/jobs/${jobs.jobs[0].id}/logs`
        );
        if (logRes.ok) {
          const logText = await logRes.text();
          const lines = logText.split('\n').filter(l => l.trim());
          // 找失败步骤附近的错误行（含 Error/error/FAILED/exception）
          const clean = lines.map(l =>
            l.replace(/^\d{4}-\d{2}-\d{2}T[\d:.]+Z /, '').replace(/\x1b\[[\d;]*m/g,'').trim()
          );
          const compileFail = clean.some(l => /Execution failed for task ':app:compileReleaseKotlin'|Compilation error|compileReleaseKotlin FAILED/i.test(l));
          if (compileFail) {
            const focused = clean.filter(l =>
              !/^##\[group\]|^##\[endgroup\]/i.test(l) &&
              !/^shell:|^env:|^with:|JAVA_HOME|ANDROID_HOME|GRADLE_USER/i.test(l)
            );
            result.failed_log = focused.slice(-220).join('\n');
          } else {
            const errLines = clean.filter(l =>
              /error|failed|exception|cannot|unable|no such|expecting|unresolved reference/i.test(l) &&
              !/^##\[group\]|^##\[endgroup\]/i.test(l)
            );
            result.failed_log = errLines.slice(-60).join('\n');
          }
        }
      } catch (_) {}
    }
  }

  return json(result);
}

async function handleLogs(request, env) {
  const p = new URL(request.url).searchParams;
  const runId = p.get('run_id'), jobId = p.get('job_id');
  if (!runId) return json({ lines: [] });
  let jid = jobId;
  if (!jid) {
    const jr = await gh(env, `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/runs/${runId}/jobs`);
    const jd = await jr.json();
    jid = jd.jobs?.[0]?.id;
  }
  if (!jid) return json({ lines: [] });
  try {
    const lr = await gh(env, `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/jobs/${jid}/logs`);
    if (!lr.ok) return json({ lines: [] });
    const raw = await lr.text();
    const lines = raw.split('\n')
      .map(l => l.replace(/^\d{4}-\d{2}-\d{2}T[\d:.]+Z /, '').replace(/\x1b\[[\d;]*m/g,'').trim())
      .filter(l => l &&
        !/^##\[group|^##\[endgroup/i.test(l) &&
        !/California|MIPS|Evaluation|Recipient|UL or FCC|Pre-Release|GOOGLE_|LIMITATION|LICENSE|jurisdic/i.test(l) &&
        !/^shell:|^env:|^with:|JAVA_HOME|ANDROID_HOME|GRADLE_USER/i.test(l)
      );
    return json({ lines: lines.slice(-150) });
  } catch(_) { return json({ lines: [] }); }
}

async function handleDownload(request, env) {
  const params     = new URL(request.url).searchParams;
  const runId      = params.get('run_id');
  const artifactId = params.get('artifact_id');
  if (!runId) return json({ error: 'Missing run_id' }, 400);

  const artsRes = await (await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/runs/${runId}/artifacts`
  )).json();
  const artifacts = (artsRes.artifacts || []).filter(a => !a.expired);
  if (!artifacts.length) {
    return json({ error: 'Artifact not found', run_id: runId, artifact_id: artifactId || null, artifacts: [] }, 404);
  }

  let picked = null;
  if (artifactId) picked = artifacts.find(a => String(a.id) === String(artifactId));
  if (!picked) picked = artifacts[0];

  const resolvedId = picked.id;
  const artifactName = picked.name || 'apk';
  const isWindows = artifactName === 'windows-x64-setup';

  const dlRedirect = await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/artifacts/${resolvedId}/zip`,
    { redirect: 'manual' }
  );
  const s3Url = dlRedirect.headers.get('location');
  if (!s3Url) {
    return json({ error: 'Download redirect failed', status: dlRedirect.status, run_id: runId, artifact_id: resolvedId }, 502);
  }

  const dl = await fetch(s3Url);
  if (!dl.ok) return json({ error: 'Download failed from S3', status: dl.status, run_id: runId, artifact_id: resolvedId }, 502);
  if (isWindows) {
    try {
      // Only accept the installer produced by the Windows workflow.
      const exe = await extractWindowsSetup(dl);
      return new Response(exe, { headers: {
        'Content-Type': 'application/octet-stream',
        'Content-Disposition': 'attachment; filename="Pakr-Windows-x64-Setup.exe"',
        'Content-Length': String(exe.byteLength),
        'Cache-Control': 'no-store',
      } });
    } catch {
      return json({ error: 'Windows 安装包无效，请重新打包' }, 502);
    }
  }
  const zipBuf = await dl.arrayBuffer();

  try {
    const apk = await extractApkFromZip(zipBuf);
    if (apk) {
      const apkName = artifactName.replace(/\.zip$/, '') + '.apk';
      return new Response(apk, {
        headers: {
          'Content-Type': 'application/vnd.android.package-archive',
          'Content-Disposition': `attachment; filename="${apkName}"`,
          'Content-Length': apk.byteLength.toString(),
          'Cache-Control': 'no-store',
        }
      });
    }
  } catch (_) {}

  return new Response(zipBuf, {
    headers: {
      'Content-Type': 'application/zip',
      'Content-Disposition': `attachment; filename="${artifactName}.zip"`,
      'Cache-Control': 'no-store',
    }
  });
}

async function extractWindowsSetup(response) {
  const maxBytes = 24 * 1024 * 1024;
  async function readLimited(stream) {
    const reader = stream.getReader();
    const chunks = [];
    let size = 0;
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        size += value.byteLength;
        if (size > maxBytes) throw new Error('Artifact too large');
        chunks.push(value);
      }
    } finally { await reader.cancel(); }
    return new Response(new Blob(chunks)).arrayBuffer();
  }

  const zip = await readLimited(response.body);
  const view = new DataView(zip);
  const end = zip.byteLength - 22;
  if (end < 0 || view.getUint32(end, true) !== 0x06054b50 || view.getUint16(end + 10, true) !== 1) {
    throw new Error('Unexpected artifact ZIP');
  }
  const entry = view.getUint32(end + 16, true);
  if (entry + 46 > end || view.getUint32(entry, true) !== 0x02014b50) throw new Error('Invalid ZIP entry');
  const bytes = new Uint8Array(zip);
  const nameLength = view.getUint16(entry + 28, true);
  const name = new TextDecoder().decode(bytes.subarray(entry + 46, entry + 46 + nameLength));
  if (name !== 'Pakr-Windows-x64-Setup.exe') throw new Error('Unexpected artifact name');
  const method = view.getUint16(entry + 10, true);
  const compressedSize = view.getUint32(entry + 20, true);
  const size = view.getUint32(entry + 24, true);
  const local = view.getUint32(entry + 42, true);
  if (size < 64 || size > maxBytes || local + 30 > entry || view.getUint32(local, true) !== 0x04034b50) {
    throw new Error('Invalid installer size or header');
  }
  const offset = local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
  if (offset + compressedSize > entry || ![0, 8].includes(method)) throw new Error('Invalid installer data');
  const payload = zip.slice(offset, offset + compressedSize);
  const exe = method === 0 ? payload : await readLimited(new Blob([payload]).stream().pipeThrough(new DecompressionStream('deflate-raw')));
  const pe = new DataView(exe);
  if (exe.byteLength !== size || pe.getUint16(0, true) !== 0x5a4d) throw new Error('Expected Windows executable');
  const peOffset = pe.getUint32(60, true);
  if (peOffset + 4 > size || pe.getUint32(peOffset, true) !== 0x00004550) throw new Error('Invalid PE header');
  return exe;
}

// 解析 ZIP Local File Headers// 解析 ZIP Local File Headers，提取第一个 .apk 文件字节（支持 stored + deflate）
async function extractApkFromZip(buf) {
  const view  = new DataView(buf);
  const bytes = new Uint8Array(buf);
  let offset  = 0;
  while (offset + 30 < bytes.length) {
    if (view.getUint32(offset, true) !== 0x04034b50) break;
    const compression = view.getUint16(offset + 8,  true);
    const compSize    = view.getUint32(offset + 18, true);
    const uncompSize  = view.getUint32(offset + 22, true);
    const nameLen     = view.getUint16(offset + 26, true);
    const extraLen    = view.getUint16(offset + 28, true);
    const name        = new TextDecoder().decode(bytes.slice(offset + 30, offset + 30 + nameLen));
    const dataOffset  = offset + 30 + nameLen + extraLen;
    if (name.endsWith('.apk')) {
      const slice = buf.slice(dataOffset, dataOffset + compSize);
      if (compression === 0) return slice;           // stored
      if (compression === 8) {                       // deflate
        const ds = new DecompressionStream('deflate-raw');
        const writer = ds.writable.getWriter();
        writer.write(new Uint8Array(slice));
        writer.close();
        return new Response(ds.readable).arrayBuffer();
      }
    }
    // fix(bug#5): 跳过可能存在的 data descriptor (0x08074b50, 最多16字节)
    let nextOffset = dataOffset + compSize;
    if (view.getUint32(nextOffset, true) === 0x08074b50) nextOffset += 16;
    offset = nextOffset;
  }
  return null;
}

function gh(env, path, opts = {}) {
  const token = [env.GITHUB_TOKEN, env.GH_TOKEN, env.GH_PAT]
    .map(value => String(value || '').trim())
    .find(Boolean);
  if (!token) throw new Error('Missing GITHUB_TOKEN');
  return fetch(`https://api.github.com${path}`, {
    ...opts,
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      'User-Agent': 'APK-Builder-CF-Worker/1.0',
      'Content-Type': 'application/json',
      ...(opts.headers || {}),
    }
  });
}

function json(d, s = 200) { return new Response(JSON.stringify(d), { status: s, headers: { 'Content-Type': 'application/json' } }); }
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
async function handleCancel(request, env) {
  const runId = new URL(request.url).searchParams.get('run_id');
  if (!runId) return json({ error: 'Missing run_id' }, 400);
  const r = await gh(env,
    `/repos/${env.GITHUB_OWNER}/${env.GITHUB_REPO}/actions/runs/${runId}/cancel`,
    { method: 'POST' }
  );
  // 202 = cancel accepted, 409 = already completed
  if (r.status === 202 || r.status === 409) return json({ ok: true });
  return json({ error: 'Cancel failed', status: r.status }, 500);
}

function cors(res, env) {
  const h = new Headers(res.headers);
  h.set('Access-Control-Allow-Origin',  '*');
  h.set('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  h.set('Access-Control-Allow-Headers', 'Content-Type');
  return new Response(res.body, { status: res.status, headers: h });
}
// force-redeploy: pages-advanced-mode-fix-20260502-1137
