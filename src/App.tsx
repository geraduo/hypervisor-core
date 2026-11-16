import { useEffect, useState } from "react";

type View = "overview" | "compatibility" | "driver" | "build";

const navItems: { id: View; label: string; number: string }[] = [
  { id: "overview", label: "Overview", number: "01" },
  { id: "compatibility", label: "Compatibility", number: "02" },
  { id: "driver", label: "Driver protocol", number: "03" },
  { id: "build", label: "Build & test", number: "04" },
];

const configureCommand = 'cmake -S . -B build -G "Visual Studio 17 2022" -A x64';
const buildCommand = "cmake --build build --config Release";
const testCommand = "ctest --test-dir build -C Release --output-on-failure";

function Icon({ name, className = "" }: { name: View; className?: string }) {
  const common = {
    className,
    viewBox: "0 0 24 24",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: 1.6,
    strokeLinecap: "round" as const,
    strokeLinejoin: "round" as const,
    "aria-hidden": true as const,
  };

  if (name === "overview") {
    return (
      <svg {...common}>
        <path d="M4 7.5 12 3l8 4.5v9L12 21l-8-4.5z" />
        <path d="m4.5 7.8 7.5 4.4 7.5-4.4M12 12.2V21" />
      </svg>
    );
  }
  if (name === "compatibility") {
    return (
      <svg {...common}>
        <path d="M12 3 20 6v5.5c0 4.5-3 7.7-8 9.5-5-1.8-8-5-8-9.5V6z" />
        <path d="m8.5 12 2.2 2.2 4.8-4.8" />
      </svg>
    );
  }
  if (name === "driver") {
    return (
      <svg {...common}>
        <rect x="4" y="4" width="16" height="6" rx="1.4" />
        <rect x="4" y="14" width="16" height="6" rx="1.4" />
        <path d="M8 7h.01M8 17h.01M12 7h4M12 17h4" />
      </svg>
    );
  }
  return (
    <svg {...common}>
      <path d="M5 19V5M5 19h15" />
      <path d="m8 15 3-4 3 2 5-7" />
      <path d="M16 6h3v3" />
    </svg>
  );
}

function BrandMark() {
  return (
    <svg className="brand-mark" viewBox="0 0 42 42" fill="none" aria-hidden="true">
      <path d="M21 3.5 37.5 13v16L21 38.5 4.5 29V13z" stroke="currentColor" strokeWidth="1.2" />
      <path d="M27.8 14.7h-8.1a5.2 5.2 0 1 0 0 10.4h4.9v-4.2h-4.1" stroke="currentColor" strokeWidth="2.2" strokeLinecap="square" />
      <path d="M27.8 14.7v6.2" stroke="currentColor" strokeWidth="2.2" />
    </svg>
  );
}

function ArrowIcon() {
  return (
    <svg viewBox="0 0 20 20" fill="none" aria-hidden="true">
      <path d="M3.5 10h12m-5-5 5 5-5 5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function CopyIcon() {
  return (
    <svg viewBox="0 0 20 20" fill="none" aria-hidden="true">
      <rect x="6.5" y="6.5" width="9" height="10" rx="1.5" stroke="currentColor" strokeWidth="1.3" />
      <path d="M12.5 6V4.7A1.7 1.7 0 0 0 10.8 3H5.7A1.7 1.7 0 0 0 4 4.7v6.1a1.7 1.7 0 0 0 1.7 1.7H6" stroke="currentColor" strokeWidth="1.3" />
    </svg>
  );
}

function SignalRow({
  index,
  title,
  detail,
  status,
  tone = "neutral",
}: {
  index: string;
  title: string;
  detail: string;
  status: string;
  tone?: "neutral" | "green" | "amber";
}) {
  return (
    <div className="signal-row">
      <span className="row-index">{index}</span>
      <div className="row-copy">
        <strong>{title}</strong>
        <span>{detail}</span>
      </div>
      <span className={`row-status status-${tone}`}>
        <i />
        {status}
      </span>
    </div>
  );
}

function CheckRow({
  label,
  detail,
  index,
}: {
  label: string;
  detail: string;
  index: string;
}) {
  return (
    <div className="check-row">
      <span className="row-index">{index}</span>
      <div>
        <h3>{label}</h3>
        <p>{detail}</p>
      </div>
      <span className="not-checked"><i />NOT CHECKED</span>
    </div>
  );
}

function useNotice() {
  const [notice, setNotice] = useState("");

  useEffect(() => {
    if (!notice) return;
    const timeout = window.setTimeout(() => setNotice(""), 2600);
    return () => window.clearTimeout(timeout);
  }, [notice]);

  return { notice, setNotice };
}

function getPreviewReport() {
  return {
    project: "Geraduo Hypervisor Core",
    version: "0.4.0",
    report_scope: "browser-preview",
    host_os_query: "not-performed",
    kernel_driver_connection: "not-connected",
    compatibility_verdict: "not-evaluated-in-browser",
    vmx_ready: false,
    notes: [
      "A browser cannot call the Windows Platform Backend or inspect kernel state.",
      "The native C++ user-mode diagnostics are required for host data.",
      "The driver is diagnostic-only and does not implement hardware virtualization.",
    ],
  };
}

export default function App() {
  const [activeView, setActiveView] = useState<View>(() => {
    const initial = window.location.hash.replace("#", "") as View;
    return navItems.some((item) => item.id === initial) ? initial : "overview";
  });
  const [lastUpdated, setLastUpdated] = useState("VIEW-ONLY");
  const { notice, setNotice } = useNotice();

  useEffect(() => {
    const handleHashChange = () => {
      const next = window.location.hash.replace("#", "") as View;
      if (navItems.some((item) => item.id === next)) setActiveView(next);
    };
    window.addEventListener("hashchange", handleHashChange);
    return () => window.removeEventListener("hashchange", handleHashChange);
  }, []);

  const selectView = (view: View) => {
    setActiveView(view);
    window.history.replaceState(null, "", `#${view}`);
  };

  const copyText = async (value: string, label: string) => {
    try {
      await navigator.clipboard.writeText(value);
      setNotice(`${label} copied`);
    } catch {
      const field = document.createElement("textarea");
      field.value = value;
      field.setAttribute("readonly", "");
      field.style.position = "fixed";
      field.style.opacity = "0";
      document.body.appendChild(field);
      field.select();
      const copied = document.execCommand("copy");
      field.remove();
      setNotice(copied ? `${label} copied` : "Clipboard access unavailable");
    }
  };

  const exportReport = () => {
    const blob = new Blob([JSON.stringify(getPreviewReport(), null, 2)], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = "geraduo-platform-preview-v0.4.0.json";
    link.click();
    URL.revokeObjectURL(url);
    setNotice("Preview report exported");
  };

  const refreshView = () => {
    setLastUpdated(
      new Intl.DateTimeFormat("en", {
        hour: "2-digit",
        minute: "2-digit",
        hour12: false,
      }).format(new Date()),
    );
    setNotice("View timestamp refreshed. Host checks were not run.");
  };

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <button className="brand-lockup" onClick={() => selectView("overview")} aria-label="Geraduo Hypervisor Core overview">
          <BrandMark />
          <span className="brand-type">
            <span className="brand-name">GERADUO</span>
            <span className="brand-product">HYPERVISOR CORE</span>
          </span>
        </button>

        <div className="sidebar-divider" />
        <div className="nav-caption">PLATFORM LAYER</div>
        <nav className="primary-nav" aria-label="Main navigation">
          {navItems.map((item) => (
            <button
              key={item.id}
              className={`nav-item ${activeView === item.id ? "is-active" : ""}`}
              onClick={() => selectView(item.id)}
              aria-current={activeView === item.id ? "page" : undefined}
            >
              <Icon name={item.id} className="nav-icon" />
              <span>{item.label}</span>
              <small>{item.number}</small>
            </button>
          ))}
        </nav>

        <div className="sidebar-bottom">
          <div className="build-mark">
            <span className="build-mark-dot" />
            <div>
              <strong>v0.4.0</strong>
              <span>WINDOWS BACKEND</span>
            </div>
          </div>
          <p>READ-ONLY DIAGNOSTICS<br />NO VMM OPERATIONS</p>
        </div>
      </aside>

      <main className="workspace">
        <header className="topbar">
          <div className="breadcrumb">
            <span>GERADUO / CORE</span>
            <b>/</b>
            <strong>{navItems.find((item) => item.id === activeView)?.label}</strong>
          </div>
          <div className="topbar-tools">
            <span className="preview-label"><i />BROWSER PREVIEW</span>
            <button className="icon-button" onClick={refreshView} aria-label="Refresh view timestamp" title="Refresh view timestamp">
              <svg viewBox="0 0 20 20" fill="none" aria-hidden="true">
                <path d="M16.3 9a6.4 6.4 0 0 0-11.7-2L3 9m0-4v4h4M3.7 11a6.4 6.4 0 0 0 11.7 2L17 11m0 4v-4h-4" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" strokeLinejoin="round" />
              </svg>
            </button>
            <button className="top-export" onClick={exportReport}>Export preview <ArrowIcon /></button>
          </div>
        </header>

        <div className="page-content" key={activeView}>
          {activeView === "overview" && (
            <OverviewView
              onOpenDriver={() => selectView("driver")}
              onExport={exportReport}
            />
          )}
          {activeView === "compatibility" && <CompatibilityView />}
          {activeView === "driver" && (
            <DriverView onCopy={(value, label) => void copyText(value, label)} />
          )}
          {activeView === "build" && (
            <BuildView onCopy={(value, label) => void copyText(value, label)} />
          )}
        </div>

        <footer className="workspace-footer">
          <span>GERADUO HYPERVISOR CORE <b>·</b> DEVELOPMENT BUILD</span>
          <span>VIEW UPDATED <strong>{lastUpdated}</strong></span>
        </footer>
      </main>

      {notice && <div className="toast" role="status">{notice}</div>}
    </div>
  );
}

function OverviewView({
  onOpenDriver,
  onExport,
}: {
  onOpenDriver: () => void;
  onExport: () => void;
}) {
  return (
    <>
      <section className="overview-hero">
        <div className="hero-copy">
          <div className="eyebrow"><span className="eyebrow-line" />GERADUO / WINDOWS PLATFORM / 0.4.0</div>
          <h1>Windows platform<br /><em>backend</em></h1>
          <p className="hero-description">
            A read-only boundary between Geraduo and the Windows host. Collect system evidence,
            surface virtualization constraints, and keep the kernel contract deliberately small.
          </p>
          <div className="hero-actions">
            <button className="button-primary" onClick={onOpenDriver}>Inspect driver contract <ArrowIcon /></button>
            <button className="button-text" onClick={onExport}>Export preview report</button>
          </div>
        </div>

        <div className="flow-figure" aria-label="User-mode diagnostics communicate with the Windows kernel through one buffered diagnostic IOCTL">
          <div className="flow-heading">
            <span>CONTROL PATH</span>
            <span>01 / 01 IOCTL</span>
          </div>
          <div className="flow-track">
            <div className="flow-node">
              <span className="node-number">01</span>
              <strong>USER MODE</strong>
              <small>platform diagnostics</small>
            </div>
            <div className="flow-connector"><span /><ArrowIcon /></div>
            <div className="flow-node flow-node-accent">
              <span className="node-number">02</span>
              <strong>METHOD_BUFFERED</strong>
              <small>size-checked request</small>
            </div>
            <div className="flow-connector"><span /><ArrowIcon /></div>
            <div className="flow-node">
              <span className="node-number">03</span>
              <strong>WDM DRIVER</strong>
              <small>platform info only</small>
            </div>
          </div>
          <div className="flow-footnote">
            <span>NO USER POINTERS</span>
            <span>NO MEMORY READ / WRITE</span>
          </div>
        </div>
      </section>

      <section className="host-summary">
        <div className="summary-label">
          <span className="summary-index">00</span>
          <span>HOST SESSION</span>
        </div>
        <div className="summary-state">
          <span className="state-orbit"><i /></span>
          <div>
            <strong>No live host connection</strong>
            <p>This browser preview cannot query Windows APIs or open a kernel device.</p>
          </div>
        </div>
        <div className="summary-aside">
          <span>LOCAL CHECKS</span>
          <strong>NOT RUN</strong>
        </div>
      </section>

      <section className="overview-details">
        <div className="section-heading">
          <div>
            <div className="eyebrow"><span className="eyebrow-line" />01 / SIGNAL MAP</div>
            <h2>What the backend observes</h2>
          </div>
          <p>Evidence is reported with its source and limits.<br />No result is inferred from browser state.</p>
        </div>
        <div className="signal-list">
          <SignalRow index="01" title="Windows host" detail="RtlGetVersion · native architecture · page size" status="Windows only" />
          <SignalRow index="02" title="CPU capability" detail="Existing CPUID provider · vendor · family · model" status="User mode" tone="green" />
          <SignalRow index="03" title="VBS / HVCI" detail="Device Guard registry configuration · not runtime state" status="Advisory" tone="amber" />
          <SignalRow index="04" title="Kernel driver" detail="One versioned, read-only platform information request" status="Disconnected" />
        </div>
      </section>

      <section className="boundary-note">
        <div className="boundary-glyph">!</div>
        <div>
          <span>BOUNDARY / NOT A READY VERDICT</span>
          <p>CPUID can report VMX, but cannot prove VMXON is permitted. This release does not implement a hardware hypervisor.</p>
        </div>
        <button onClick={onOpenDriver} aria-label="Read diagnostic driver boundary"><ArrowIcon /></button>
      </section>
    </>
  );
}

function CompatibilityView() {
  return (
    <section className="detail-page">
      <div className="detail-intro">
        <div className="eyebrow"><span className="eyebrow-line" />02 / PLATFORM COMPATIBILITY</div>
        <h1>Evidence, <em>not a green light.</em></h1>
        <p>
          Compatibility is a structured review of the host. It does not establish ownership of
          virtualization features or claim that kernel initialization will succeed.
        </p>
      </div>

      <div className="verdict-band">
        <span className="verdict-dot" />
        <strong>NO LIVE VERDICT</strong>
        <span>Browser preview has no access to Windows host state.</span>
      </div>

      <div className="detail-section-title">
        <span>CHECK SURFACE</span>
        <span>SOURCE / CONFIDENCE</span>
      </div>
      <div className="check-list">
        <CheckRow
          index="01"
          label="Hyper-V / hypervisor presence"
          detail="CPUID hypervisor bit and vendor string can identify a hypervisor environment. They cannot prove a conflict or distinguish every Windows configuration."
        />
        <CheckRow
          index="02"
          label="Virtualization-based Security"
          detail="Device Guard registry configuration is read-only evidence. An enabled value is not equivalent to VBS being active after boot."
        />
        <CheckRow
          index="03"
          label="Hypervisor-protected Code Integrity"
          detail="HVCI configuration is reported separately from runtime status. The current backend does not query Win32_DeviceGuard through WMI."
        />
        <CheckRow
          index="04"
          label="Kernel VMX checks"
          detail="IA32_FEATURE_CONTROL, VMX capability MSRs, CR0/CR4 fixed bits, EPT capability, and VMXON are not queried or executed."
        />
      </div>

      <div className="method-note">
        <span className="note-marker">METHOD</span>
        <p>Known Hyper-V vendors are identified from CPUID. Any hypervisor-present bit remains a review signal, not an automatic incompatibility finding.</p>
      </div>
    </section>
  );
}

function DriverView({ onCopy }: { onCopy: (value: string, label: string) => void }) {
  return (
    <section className="detail-page driver-page">
      <div className="detail-intro">
        <div className="eyebrow"><span className="eyebrow-line" />03 / USER MODE ↔ KERNEL MODE</div>
        <h1>One request.<br /><em>One narrow answer.</em></h1>
        <p>
          A PnP WDM diagnostic device with a versioned, buffered protocol. The interface grants
          read access to Administrators and SYSTEM; no arbitrary memory operation exists.
        </p>
      </div>

      <div className="ioctl-signature">
        <div>
          <span className="signature-caption">DIAGNOSTIC IOCTL</span>
          <strong>IOCTL_GERADUO_QUERY_PLATFORM_INFO</strong>
          <small>CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_READ_DATA)</small>
        </div>
        <button className="copy-button" onClick={() => onCopy("IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x80006000", "IOCTL definition")}>
          <CopyIcon /> Copy
        </button>
      </div>

      <div className="protocol-grid">
        <div className="protocol-value"><span>PROTOCOL VERSION</span><strong>01</strong><small>explicitly checked</small></div>
        <div className="protocol-value"><span>REQUEST STRUCT</span><strong>16 <i>bytes</i></strong><small>version · size · request id</small></div>
        <div className="protocol-value"><span>RESPONSE STRUCT</span><strong>48 <i>bytes</i></strong><small>status + platform info</small></div>
        <div className="protocol-value"><span>TRANSFER METHOD</span><strong>BUFFERED</strong><small>no user pointers</small></div>
      </div>

      <div className="driver-columns">
        <div>
          <div className="detail-section-title"><span>DEVICE RESPONSE</span><span>READ ONLY</span></div>
          <div className="response-lines">
            <div><code>01</code><span>Windows version and build</span></div>
            <div><code>02</code><span>Native architecture</span></div>
            <div><code>03</code><span>Logical processor count</span></div>
            <div><code>04</code><span>Page size and CPUID flags</span></div>
          </div>
        </div>
        <div className="safety-aside">
          <span>SAFETY CONTRACT</span>
          <p>Input length, output length, structure size, protocol version, and request id are validated before the response is returned.</p>
          <div className="safety-tags"><span>NO METHOD_NEITHER</span><span>NO PHYSICAL MEMORY</span><span>NO PROCESS MEMORY</span></div>
        </div>
      </div>
    </section>
  );
}

function BuildView({ onCopy }: { onCopy: (value: string, label: string) => void }) {
  const commands = [
    { label: "Configure", command: configureCommand, note: "Visual Studio 2022 · x64" },
    { label: "Build user-mode components", command: buildCommand, note: "Core · diagnostics tool · tests" },
    { label: "Run test suite", command: testCommand, note: "CTest · Release configuration" },
  ];

  return (
    <section className="detail-page build-page">
      <div className="detail-intro">
        <div className="eyebrow"><span className="eyebrow-line" />04 / BUILD & TEST</div>
        <h1>Build the part<br /><em>we can verify.</em></h1>
        <p>
          CMake covers the portable user-mode library and its fakes. The WDM driver is a separate
          Visual Studio project and requires a Windows Driver Kit installation.
        </p>
      </div>

      <div className="build-status-line">
        <span className="status-chip"><i />USER-MODE SUITES</span>
        <strong>4 CTest targets</strong>
        <span>Driver build and Windows execution are not verified in this environment.</span>
      </div>

      <div className="command-list">
        {commands.map((item, index) => (
          <div className="command-row" key={item.label}>
            <span className="command-number">0{index + 1}</span>
            <div className="command-body">
              <div className="command-meta"><strong>{item.label}</strong><span>{item.note}</span></div>
              <code>{item.command}</code>
            </div>
            <button className="copy-button" onClick={() => onCopy(item.command, item.label)}>
              <CopyIcon /> Copy
            </button>
          </div>
        ))}
      </div>

      <div className="wdk-instructions">
        <div>
          <span className="eyebrow"><span className="eyebrow-line" />SEPARATE DRIVER BUILD</span>
          <h2>Visual Studio 2022 + WDK</h2>
          <p>Open <code>driver/GeraduoDriver.vcxproj</code>, select <strong>Release | x64</strong>, and build. No driver success is inferred from CMake user-mode tests.</p>
        </div>
        <button className="button-primary" onClick={() => onCopy("msbuild driver\\GeraduoDriver.vcxproj /m /p:Configuration=Release /p:Platform=x64", "WDK build command")}>
          Copy WDK command <ArrowIcon />
        </button>
      </div>
    </section>
  );
}