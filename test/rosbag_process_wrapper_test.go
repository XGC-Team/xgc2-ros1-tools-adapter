package recorder

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestROSBagProcessWrapperFinalizesManifestOnRequestedStop(t *testing.T) {
	script := rosBagProcessWrapperScript(t)
	rosRoot := fakeROSBagRuntime(t)
	outputDirectory := filepath.Join(t.TempDir(), "Experiments", "recording")
	outputPrefix := filepath.Join(outputDirectory, "xgc")
	command := rosBagProcessWrapperCommand(script, rosRoot, outputPrefix)
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	waitForFile(t, outputPrefix+".ready")
	if err := command.Process.Signal(syscall.SIGTERM); err != nil {
		t.Fatal(err)
	}
	if err := command.Wait(); err != nil {
		t.Fatalf("requested stop must be handled as a clean wrapper exit: %v", err)
	}

	manifest := readROSBagWrapperManifest(t, outputDirectory)
	if manifest.Status != "stopped" ||
		manifest.CapacityLimitReached ||
		manifest.RecorderExitCode != 7 ||
		manifest.TerminationSignal == nil ||
		*manifest.TerminationSignal != int(syscall.SIGTERM) {
		t.Fatalf("stopped manifest has incorrect termination semantics: %+v", manifest)
	}
	wantFiles := []rosBagWrapperFile{{
		Name: "xgc_0.bag", State: "finalized", SizeBytes: 8,
	}}
	if !reflect.DeepEqual(manifest.BagFiles, wantFiles) {
		t.Fatalf("stopped manifest bag files=%+v want %+v", manifest.BagFiles, wantFiles)
	}
	assertNoROSBagManifestTemporary(t, outputDirectory)
}

func TestROSBagProcessWrapperListsRecoverableActiveBagAfterFailure(t *testing.T) {
	script := rosBagProcessWrapperScript(t)
	rosRoot := fakeROSBagRuntime(t)
	outputDirectory := filepath.Join(t.TempDir(), "Experiments", "recording")
	outputPrefix := filepath.Join(outputDirectory, "xgc")
	command := rosBagProcessWrapperCommand(script, rosRoot, outputPrefix)
	command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=fail")
	err := command.Run()
	var exitError *exec.ExitError
	if !errors.As(err, &exitError) || exitError.ExitCode() != 9 {
		t.Fatalf("failing fake rosbag wrapper error=%v, want exit 9", err)
	}

	manifest := readROSBagWrapperManifest(t, outputDirectory)
	if manifest.Status != "failed" ||
		manifest.RecorderExitCode != 9 ||
		manifest.TerminationSignal != nil {
		t.Fatalf("failed manifest has incorrect termination semantics: %+v", manifest)
	}
	wantFiles := []rosBagWrapperFile{{
		Name: "xgc_0.bag.active", State: "partial", SizeBytes: 8,
	}}
	if !reflect.DeepEqual(manifest.BagFiles, wantFiles) {
		t.Fatalf("failed manifest bag files=%+v want %+v", manifest.BagFiles, wantFiles)
	}
	assertNoROSBagManifestTemporary(t, outputDirectory)
}

func TestROSBagProcessWrapperPreservesAllSplitsForEveryProfile(t *testing.T) {
	script := rosBagProcessWrapperScript(t)
	rosRoot := fakeROSBagRuntime(t)
	for _, test := range []struct {
		name      string
		profile   string
		wantLimit int64
	}{
		{name: "scientific preserves every split", profile: "camera_scientific", wantLimit: 10 << 30},
		{name: "general preserves every split", profile: "general", wantLimit: 128 << 20},
	} {
		t.Run(test.name, func(t *testing.T) {
			outputDirectory := filepath.Join(t.TempDir(), "Experiments", "recording")
			outputPrefix := filepath.Join(outputDirectory, "xgc")
			command := rosBagProcessWrapperCommandForProfile(
				script,
				rosRoot,
				outputPrefix,
				test.profile,
			)
			command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=fail")
			err := command.Run()
			var exitError *exec.ExitError
			if !errors.As(err, &exitError) || exitError.ExitCode() != 9 {
				t.Fatalf("failing fake rosbag wrapper error=%v, want exit 9", err)
			}
			raw, err := os.ReadFile(outputPrefix + ".argv.json")
			if err != nil {
				t.Fatal(err)
			}
			var arguments []string
			if err := json.Unmarshal(raw, &arguments); err != nil {
				t.Fatalf("decode fake rosbag argv: %v", err)
			}
			for _, required := range []string{"--split", "--size=64", "--min-space=1G"} {
				if !containsROSBagArgument(arguments, required) {
					t.Errorf("%s argv lacks %q: %v", test.profile, required, arguments)
				}
			}
			for _, argument := range arguments {
				if strings.HasPrefix(argument, "--max-splits=") {
					t.Fatalf("%s must not delete earlier splits: %v", test.profile, arguments)
				}
			}
			manifest := readROSBagWrapperManifest(t, outputDirectory)
			if manifest.CapacityPreflight.SplitPolicy != "preserve-all" || manifest.CapacityPreflight.StopLimitBytes != test.wantLimit || manifest.CapacityLimitReached {
				t.Fatalf("%s misreported its capacity policy or native failure reason: %+v", test.profile, manifest)
			}
		})
	}
}

type rosBagWrapperManifest struct {
	Finalization         string              `json:"archiveFinalizationState"`
	FinalizationError    string              `json:"archiveFinalizationError"`
	Status               string              `json:"status"`
	RecorderExitCode     int                 `json:"recorderExitCode"`
	TerminationSignal    *int                `json:"terminationSignal"`
	CapacityLimitReached bool                `json:"capacityLimitReached"`
	BagFiles             []rosBagWrapperFile `json:"bagFiles"`
	CapacityPreflight    struct {
		SplitPolicy    string `json:"splitPolicy"`
		StopLimitBytes int64  `json:"stopLimitBytes"`
	} `json:"capacityPreflight"`
}

type rosBagWrapperFile struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	State     string `json:"state"`
	SizeBytes int64  `json:"sizeBytes"`
}

func rosBagProcessWrapperScript(t *testing.T) string {
	t.Helper()
	_, file, _, _ := runtime.Caller(0)
	raw, err := os.ReadFile(filepath.Join(filepath.Dir(file), "..", "scripts", "rosbag_recorder.py"))
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	// The old runtime cases exercise the same production runtime body with
	// prepared inputs. Public stdin preparation is exercised independently.
	first := strings.Index(script, "try:\n    if sys.argv[1:] == [\"--prepare\"]:")
	last := strings.Index(script, "\ndef require_archive_path")
	if first < 0 || last < first {
		t.Fatal("production preparation boundary missing")
	}
	return script[:first] + script[last:]
}

func fakeROSBagRuntime(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	bin := filepath.Join(root, "lib", "rosbag")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	const script = `#!/usr/bin/python3
import json
import os
import signal
import sys
import time

prefix_option = "--output-name" if "--output-name" in sys.argv else "--output-prefix"
prefix = sys.argv[sys.argv.index(prefix_option) + 1]
with open(prefix + ".argv.json", "w", encoding="utf-8") as stream:
    json.dump(sys.argv[1:], stream)
active = prefix + "_0.bag.active"
with open(active, "wb") as stream:
    stream.write(b"bag-data")
    stream.flush()
    os.fsync(stream.fileno())
if os.environ.get("XGC_TEST_ROSBAG_MODE") == "fail":
    raise SystemExit(9)
if os.environ.get("XGC_TEST_ROSBAG_MODE") == "complete":
    os.replace(active, prefix + "_0.bag")
    raise SystemExit(0)
def stop(_signum, _frame):
    if os.path.exists(active):
        os.replace(active, active[:-len(".active")])
    raise SystemExit(7)

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)
if os.environ.get("XGC_TEST_ROSBAG_MODE") == "grow":
    chunk = b"x" * (1024 * 1024)
    while True:
        with open(active, "ab") as stream:
            stream.write(chunk)
            stream.flush()
        time.sleep(0.01)
if os.environ.get("XGC_TEST_ROSBAG_MODE") == "two-splits":
    signal.alarm(10)
    # Sparse data models native segmentation without writing 128 MiB per test.
    with open(active, "ab") as stream:
        stream.truncate(64 * 1024 * 1024)
    os.replace(active, prefix + "_0.bag")
    active = prefix + "_1.bag.active"
    with open(active, "wb") as stream:
        stream.write(b"second-bag")
        stream.truncate(64 * 1024 * 1024)
        stream.flush()
        os.fsync(stream.fileno())
with open(prefix + ".ready", "wb"):
    pass
while True:
    time.sleep(0.05)
`
	if err := os.WriteFile(filepath.Join(bin, "record"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return root
}

func TestROSBagProcessWrapperStopsScientificAtPlannedCapacity(t *testing.T) {
	script := rosBagProcessWrapperScript(t)
	rosRoot := fakeROSBagRuntime(t)
	outputDirectory := filepath.Join(t.TempDir(), "Experiments", "recording")
	outputPrefix := filepath.Join(outputDirectory, "xgc")
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	command := rosBagProcessWrapperCommandWithCapacity(
		ctx, script, rosRoot, outputPrefix, "camera_scientific", "64", "1",
		64*1024*1024,
	)
	command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=grow")
	if err := command.Run(); err != nil {
		t.Fatalf("scientific capacity stop must exit 0: %v", err)
	}
	manifest := readROSBagWrapperManifest(t, outputDirectory)
	if !manifest.CapacityLimitReached || manifest.Status != "stopped" || manifest.RecorderExitCode != 7 || manifest.CapacityPreflight.StopLimitBytes != 64<<20 || manifest.CapacityPreflight.SplitPolicy != "preserve-all" {
		t.Fatalf("scientific capacity stop manifest=%+v", manifest)
	}
	if manifest.TerminationSignal == nil || *manifest.TerminationSignal != int(syscall.SIGTERM) {
		t.Fatalf("scientific capacity stop must SIGTERM rosbag: %+v", manifest)
	}
	total := int64(0)
	for _, file := range manifest.BagFiles {
		if file.State != "finalized" {
			t.Fatalf("capacity stop did not wait for native finalization: %+v", file)
		}
		total += file.SizeBytes
	}
	if total < 64*1024*1024 {
		t.Fatalf("scientific capacity stop recorded %d bytes, want at least 64 MiB", total)
	}
}

func TestROSBagProcessWrapperStopsGeneralAtCapacityAndPreservesEarlierSplits(t *testing.T) {
	// The current wrapper must preserve data for both current and historical
	// parameter shapes; immutable older pinned wrapper versions are unchanged.
	for _, namingVersion := range []int{0, 1} {
		t.Run(fmt.Sprint("naming-version=", namingVersion), func(t *testing.T) {
			prefix := filepath.Join(t.TempDir(), "Experiments", "recording", "四车_仿真_2026-09-20_18-30-00")
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			command := rosBagProcessWrapperCommandWithCapacity(ctx, rosBagProcessWrapperScript(t), fakeROSBagRuntime(t), prefix, "general", "64", "2", 64<<20)
			command.Args[len(command.Args)-1] = rosBagWrapperSessionManifestJSON("general", map[string]any{
				"recordNamingVersion": namingVersion, "recordingId": readableRecordID,
			})
			command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=two-splits")
			if output, err := command.CombinedOutput(); err != nil {
				t.Fatalf("general capacity stop: %v %s", err, output)
			}
			manifest := readROSBagWrapperManifest(t, filepath.Dir(prefix))
			if !manifest.CapacityLimitReached || manifest.Status != "stopped" || manifest.RecorderExitCode != 7 || manifest.TerminationSignal == nil || *manifest.TerminationSignal != int(syscall.SIGTERM) {
				t.Fatalf("capacity stop lost native completion or its reason: %+v", manifest)
			}
			if manifest.CapacityPreflight.SplitPolicy != "preserve-all" || manifest.CapacityPreflight.StopLimitBytes != 128<<20 || len(manifest.BagFiles) != 2 {
				t.Fatalf("general capacity or preserved split inventory is incorrect: %+v", manifest)
			}
			for index, file := range manifest.BagFiles {
				wantName := fmt.Sprintf("%s_%d.bag", filepath.Base(prefix), index)
				wantID := ""
				if namingVersion == 1 {
					wantName = fmt.Sprintf("%s-part%02d.bag", filepath.Base(prefix), index+1)
					wantID = fmt.Sprintf("bag.%s.%d", readableRecordID, index)
				}
				if file.Name != wantName || file.ID != wantID || file.State != "finalized" || file.SizeBytes != 64<<20 {
					t.Fatalf("split %d changed or was not finalized: %+v", index, file)
				}
				path := filepath.Join(filepath.Dir(prefix), file.Name)
				info, err := os.Stat(path)
				if err != nil || info.Size() != file.SizeBytes {
					t.Fatalf("split %d is absent or truncated: %v", index, err)
				}
				stream, err := os.Open(path)
				if err != nil {
					t.Fatal(err)
				}
				firstBytes := make([]byte, 8)
				_, readErr := stream.Read(firstBytes)
				_ = stream.Close()
				wantBytes := []string{"bag-data", "second-b"}[index]
				if readErr != nil || string(firstBytes) != wantBytes {
					t.Fatalf("split %d bytes changed: %q %v", index, firstBytes, readErr)
				}
			}
			arguments, err := os.ReadFile(prefix + ".argv.json")
			if err != nil || strings.Contains(string(arguments), "--max-splits") {
				t.Fatalf("native retention may delete raw data: %s %v", arguments, err)
			}
			assertNoROSBagManifestTemporary(t, filepath.Dir(prefix))
		})
	}
}

func rosBagProcessWrapperCommand(script, rosRoot, outputPrefix string) *exec.Cmd {
	return rosBagProcessWrapperCommandForProfile(script, rosRoot, outputPrefix, "camera_scientific")
}

func rosBagWrapperSessionManifestJSON(recordingProfile string, extra map[string]any) string {
	document := map[string]any{
		"schemaVersion":    1,
		"recordingProfile": recordingProfile,
		"topics":           []string{"/clock"},
		"estimatedBytes":   0,
	}
	if recordingProfile == "camera_scientific" {
		document["scientificTotalLimitBytes"] = 10 << 30
	}
	for key, value := range extra {
		document[key] = value
	}
	raw, err := json.Marshal(document)
	if err != nil {
		panic(err)
	}
	return string(raw)
}

func rosBagProcessWrapperCommandWithCapacity(
	ctx context.Context,
	script, rosRoot, outputPrefix, recordingProfile, splitSize, maxSplits string,
	scientificTotalLimitBytes int64,
) *exec.Cmd {
	topics := `["/clock"]`
	manifest := rosBagWrapperSessionManifestJSON(recordingProfile, map[string]any{
		"scientificTotalLimitBytes": scientificTotalLimitBytes,
	})
	return isolateROSBagWrapperPython(exec.CommandContext(
		ctx,
		"/usr/bin/python3", "-c", script,
		rosRoot, outputPrefix, topics,
		splitSize, maxSplits, "1", "none", "true",
		"1", "0", "1.25", manifest,
	))
}

func rosBagProcessWrapperCommandForProfile(
	script string,
	rosRoot string,
	outputPrefix string,
	recordingProfile string,
) *exec.Cmd {
	topics := `["/clock"]`
	return isolateROSBagWrapperPython(exec.Command(
		"/usr/bin/python3", "-c", script,
		rosRoot, outputPrefix, topics,
		"64", "2", "1", "none", "true",
		"1", "0", "1.25", rosBagWrapperSessionManifestJSON(recordingProfile, nil),
	))
}

func isolateROSBagWrapperPython(command *exec.Cmd) *exec.Cmd {
	prefix := command.Args[4]
	marker := string(os.PathSeparator) + "Experiments" + string(os.PathSeparator)
	at := strings.Index(prefix, marker)
	if at < 0 {
		panic("wrapper fixture must select an explicit Experiments archive")
	}
	command.Env = append(rosBagWrapperTestEnv(), "XGC_USER_FILES_DIR="+prefix[:at])
	return command
}

func rosBagWrapperTestEnv(pythonPathPrefix ...string) []string {
	_, file, _, ok := runtime.Caller(0)
	isolated := ""
	if ok {
		isolated = filepath.Join(filepath.Dir(file), "testdata", "norecordfactsros")
	}
	pythonPath := isolated
	if len(pythonPathPrefix) > 0 && pythonPathPrefix[0] != "" {
		pythonPath = pythonPathPrefix[0] + string(os.PathListSeparator) + isolated
	}
	if existing := os.Getenv("PYTHONPATH"); existing != "" {
		pythonPath += string(os.PathListSeparator) + existing
	}
	return append(os.Environ(),
		"PYTHONPATH="+pythonPath,
		"ROS_MASTER_URI=http://127.0.0.1:1",
	)
}

func containsROSBagArgument(arguments []string, wanted string) bool {
	for _, argument := range arguments {
		if argument == wanted {
			return true
		}
	}
	return false
}

func waitForFile(t *testing.T, path string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Stat(path); err == nil {
			return
		} else if !errors.Is(err, os.ErrNotExist) {
			t.Fatal(err)
		}
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", path)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func readROSBagWrapperManifest(t *testing.T, directory string) rosBagWrapperManifest {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(directory, "session-manifest.json"))
	if err != nil {
		t.Fatal(err)
	}
	var manifest rosBagWrapperManifest
	if err := json.Unmarshal(raw, &manifest); err != nil {
		t.Fatalf("session manifest is not complete JSON: %v\n%s", err, raw)
	}
	return manifest
}

func assertNoROSBagManifestTemporary(t *testing.T, directory string) {
	t.Helper()
	temporaries, err := filepath.Glob(filepath.Join(directory, "session-manifest.json.part.*"))
	if err != nil {
		t.Fatal(err)
	}
	if len(temporaries) != 0 {
		t.Fatalf("atomic manifest temporary files remain: %v", temporaries)
	}
}

func TestROSBagWrapperAllTopicsUsesDynamicGraphSelection(t *testing.T) {
	for _, all := range []bool{false, true} {
		prefix := filepath.Join(t.TempDir(), "Experiments", "recording", "xgc")
		command := rosBagProcessWrapperCommandForProfile(rosBagProcessWrapperScript(t), fakeROSBagRuntime(t), prefix, "camera_scientific")
		var manifest map[string]any
		last := len(command.Args) - 1
		if err := json.Unmarshal([]byte(command.Args[last]), &manifest); err != nil {
			t.Fatal(err)
		}
		manifest["recordAllTopics"] = all
		encoded, err := json.Marshal(manifest)
		if err != nil {
			t.Fatal(err)
		}
		command.Args[last] = string(encoded)
		command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=fail")
		err = command.Run()
		var exitError *exec.ExitError
		if !errors.As(err, &exitError) || exitError.ExitCode() != 9 {
			t.Fatalf("unexpected recorder exit: %v", err)
		}
		raw, err := os.ReadFile(prefix + ".argv.json")
		if err != nil {
			t.Fatal(err)
		}
		var args []string
		if err := json.Unmarshal(raw, &args); err != nil {
			t.Fatal(err)
		}
		if containsROSBagArgument(args, "--all") != all || containsROSBagArgument(args, "/clock") == all {
			t.Fatalf("all=%t: wrong topic selection: %v", all, args)
		}
		for _, arg := range args {
			if strings.HasPrefix(arg, "--max-splits=") {
				t.Fatal("scientific full recording must not delete earlier splits")
			}
		}
	}
}

// rosBagWrapperArgvWithManifest runs the wrapper with a fake recorder that
// fails after recording its argv, and returns that argv.
func rosBagWrapperArgvWithManifest(t *testing.T, edit func(map[string]any)) ([]string, error) {
	t.Helper()
	prefix := filepath.Join(t.TempDir(), "Experiments", "recording", "xgc")
	command := rosBagProcessWrapperCommandForProfile(rosBagProcessWrapperScript(t), fakeROSBagRuntime(t), prefix, "camera_scientific")
	var manifest map[string]any
	last := len(command.Args) - 1
	if err := json.Unmarshal([]byte(command.Args[last]), &manifest); err != nil {
		t.Fatal(err)
	}
	edit(manifest)
	encoded, err := json.Marshal(manifest)
	if err != nil {
		t.Fatal(err)
	}
	command.Args[last] = string(encoded)
	topics, err := json.Marshal(manifest["topics"])
	if err != nil {
		t.Fatal(err)
	}
	command.Args[5] = string(topics)
	command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=fail")
	output, runErr := command.CombinedOutput()
	var exitError *exec.ExitError
	if !errors.As(runErr, &exitError) || exitError.ExitCode() != 9 {
		return nil, fmt.Errorf("recorder did not start: %v: %s", runErr, output)
	}
	raw, err := os.ReadFile(prefix + ".argv.json")
	if err != nil {
		t.Fatal(err)
	}
	var args []string
	if err := json.Unmarshal(raw, &args); err != nil {
		t.Fatal(err)
	}
	return args, nil
}

func TestROSBagWrapperAllTopicsNeverSubscribesExcludedTopics(t *testing.T) {
	excluded := []any{
		map[string]any{"topic": "/usb_cam/video_h264", "reason": "derived preview"},
		map[string]any{"topic": "/a.b/c", "reason": "regex metacharacters stay literal"},
	}
	args, err := rosBagWrapperArgvWithManifest(t, func(manifest map[string]any) {
		manifest["recordAllTopics"] = true
		manifest["excludedTopics"] = excluded
	})
	if err != nil {
		t.Fatal(err)
	}
	index := -1
	for position, argument := range args {
		if argument == "--exclude" {
			index = position
		}
	}
	if !containsROSBagArgument(args, "--all") || index < 0 || index+1 >= len(args) ||
		args[index+1] != `^(?:/usb_cam/video_h264|/a\.b/c|/xgc/display(?:/.*)?)$` {
		t.Fatalf("record-all did not exclude the authored topics exactly: %v", args)
	}

	explicit, err := rosBagWrapperArgvWithManifest(t, func(manifest map[string]any) {
		manifest["excludedTopics"] = excluded
	})
	if err != nil {
		t.Fatal(err)
	}
	if containsROSBagArgument(explicit, "--exclude") || containsROSBagArgument(explicit, "--all") {
		t.Fatalf("explicit topic recording must not add a graph-wide exclusion: %v", explicit)
	}
}

func TestROSBagWrapperRejectsInvalidOrContradictoryExclusions(t *testing.T) {
	for name, excluded := range map[string]any{
		"recorded topic excluded": []any{map[string]any{"topic": "/clock", "reason": "contradiction"}},
		"missing reason":          []any{map[string]any{"topic": "/usb_cam/video_h264"}},
		"relative topic":          []any{map[string]any{"topic": "usb_cam/video_h264", "reason": "relative"}},
		"not a list":              "/usb_cam/video_h264",
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := rosBagWrapperArgvWithManifest(t, func(manifest map[string]any) {
				manifest["recordAllTopics"] = true
				manifest["excludedTopics"] = excluded
			}); err == nil {
				t.Fatal("the wrapper started the recorder with invalid exclusions")
			}
		})
	}
}

func TestROSBagWrapperAllTopicsKeepsExplicitDisplayCopy(t *testing.T) {
	args, err := rosBagWrapperArgvWithManifest(t, func(manifest map[string]any) {
		manifest["recordAllTopics"] = true
		manifest["topics"] = []string{"/clock", "/xgc/display/robot/map"}
	})
	if err != nil {
		t.Fatal(err)
	}
	for index, argument := range args {
		if argument != "--exclude" || index+1 == len(args) {
			continue
		}
		command := exec.Command("python3", "-c", `import re,sys
pattern=re.compile(sys.argv[1])
assert pattern.fullmatch('/xgc/display/robot/path')
assert not pattern.fullmatch('/xgc/display/robot/map')
assert not pattern.fullmatch('/robot/map')
assert not pattern.fullmatch('/clock')`, args[index+1])
		if output, err := command.CombinedOutput(); err != nil {
			t.Fatalf("display exclusions lost scientific sources or an explicit copy: %v: %s", err, output)
		}
		return
	}
	t.Fatal("record-all did not exclude derived display copies")
}

const readableRecordID = "0123456789abcdef0123456789abcdef"

func readableROSBagCommand(t *testing.T, rosRoot, prefix string) *exec.Cmd {
	t.Helper()
	command := rosBagProcessWrapperCommand(rosBagProcessWrapperScript(t), rosRoot, prefix)
	command.Args[len(command.Args)-1] = rosBagWrapperSessionManifestJSON("camera_scientific", map[string]any{
		"recordNamingVersion": 1, "recordingId": readableRecordID,
	})
	return command
}

func TestROSBagReadableSegmentsIssueIdentityAfterNativeStop(t *testing.T) {
	prefix := filepath.Join(t.TempDir(), "Experiments", "四车[试验]_仿真_2026-09-20_18-30-00")
	command := readableROSBagCommand(t, fakeROSBagRuntime(t), prefix)
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	waitForFile(t, prefix+".ready")
	before := readROSBagWrapperManifest(t, filepath.Dir(prefix))
	if len(before.BagFiles) != 0 || before.Finalization != "" {
		t.Fatalf("running recorder issued files early: %+v", before)
	}
	if err := command.Process.Signal(syscall.SIGTERM); err != nil {
		t.Fatal(err)
	}
	if err := command.Wait(); err != nil {
		t.Fatal(err)
	}
	manifest := readROSBagWrapperManifest(t, filepath.Dir(prefix))
	want := []rosBagWrapperFile{{ID: "bag." + readableRecordID + ".0", Name: filepath.Base(prefix) + ".bag", State: "finalized", SizeBytes: 8}}
	if manifest.Finalization != "finalized" || !reflect.DeepEqual(manifest.BagFiles, want) {
		t.Fatalf("finalized manifest=%+v", manifest)
	}
	if _, err := os.Stat(prefix + "_0.bag"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("native temporary name survives: %v", err)
	}
	if _, err := os.Stat(prefix + "-part01.bag"); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("single volume still uses a part suffix: %v", err)
	}
	raw, err := os.ReadFile(prefix + ".bag")
	if err != nil || string(raw) != "bag-data" {
		t.Fatalf("saved bytes=%q err=%v", raw, err)
	}
	argv, err := os.ReadFile(prefix + ".argv.json")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(argv), "--output-name") || strings.Contains(string(argv), "--output-prefix") {
		t.Fatalf("native recorder still appends another timestamp: %s", argv)
	}
}

func TestROSBagReadableSegmentsKeepPartialBytesWithoutFileIdentity(t *testing.T) {
	prefix := filepath.Join(t.TempDir(), "Experiments", "四车_仿真_2026-09-20_18-30-00")
	command := readableROSBagCommand(t, fakeROSBagRuntime(t), prefix)
	command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=fail")
	var exit *exec.ExitError
	if err := command.Run(); !errors.As(err, &exit) || exit.ExitCode() != 9 {
		t.Fatalf("native error lost: %v", err)
	}
	manifest := readROSBagWrapperManifest(t, filepath.Dir(prefix))
	want := []rosBagWrapperFile{{Name: filepath.Base(prefix) + "_0.bag.active", State: "partial", SizeBytes: 8}}
	if !reflect.DeepEqual(manifest.BagFiles, want) || manifest.Status != "failed" {
		t.Fatalf("partial bytes misreported: %+v", manifest)
	}
}

func TestROSBagReadableRenameDoesNotReplaceForeignFile(t *testing.T) {
	prefix := filepath.Join(t.TempDir(), "Experiments", "四车_仿真_2026-09-20_18-30-00")
	command := readableROSBagCommand(t, fakeROSBagRuntime(t), prefix)
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	waitForFile(t, prefix+".ready")
	foreign := prefix + ".bag"
	if err := os.WriteFile(foreign, []byte("foreign"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := command.Process.Signal(syscall.SIGTERM); err != nil {
		t.Fatal(err)
	}
	if err := command.Wait(); err == nil {
		t.Fatal("foreign destination was accepted")
	}
	manifest := readROSBagWrapperManifest(t, filepath.Dir(prefix))
	if manifest.Finalization != "failed" || manifest.FinalizationError == "" || len(manifest.BagFiles) != 0 {
		t.Fatalf("failed rename was not preserved: %+v", manifest)
	}
	for path, want := range map[string]string{foreign: "foreign", prefix + "_0.bag": "bag-data"} {
		raw, err := os.ReadFile(path)
		if err != nil || string(raw) != want {
			t.Fatalf("%s bytes=%q err=%v", path, raw, err)
		}
	}
}

func TestROSBagReadableRenameRecoversExactInodeWithoutRestartingRecorder(t *testing.T) {
	for _, test := range []struct {
		unlinkDone    bool
		nativeFailure bool
	}{{}, {unlinkDone: true}, {nativeFailure: true}} {
		t.Run(fmt.Sprint("unlink=", test.unlinkDone, "/native-failure=", test.nativeFailure), func(t *testing.T) {
			prefix := filepath.Join(t.TempDir(), "Experiments", "四车_仿真_2026-09-20_18-30-00")
			source, destination := prefix+"_0.bag", prefix+".bag"
			if err := os.MkdirAll(filepath.Dir(prefix), 0755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(source, []byte("bag-data"), 0644); err != nil {
				t.Fatal(err)
			}
			if err := os.Link(source, destination); err != nil {
				t.Fatal(err)
			}
			info, err := os.Stat(source)
			if err != nil {
				t.Fatal(err)
			}
			stat := info.Sys().(*syscall.Stat_t)
			if test.unlinkDone {
				if err := os.Remove(source); err != nil {
					t.Fatal(err)
				}
			}
			document := map[string]any{"schemaVersion": 1, "recordingId": readableRecordID, "recordNamingVersion": 1, "status": "stopped", "recordingEndedAt": "2026-09-20T18:31:00Z", "archiveFinalizationState": "pending", "archiveSegments": []map[string]any{{"sourceName": filepath.Base(source), "name": filepath.Base(destination), "index": 0, "device": stat.Dev, "inode": stat.Ino, "sizeBytes": info.Size()}}}
			if test.nativeFailure {
				document["status"], document["recorderExitCode"] = "failed", 9
			} else {
				document["terminationSignal"], document["recorderExitCode"] = int(syscall.SIGTERM), 7
			}
			raw, err := json.Marshal(document)
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(filepath.Dir(prefix), "session-manifest.json"), raw, 0644); err != nil {
				t.Fatal(err)
			}
			command := readableROSBagCommand(t, fakeROSBagRuntime(t), prefix)
			output, err := command.CombinedOutput()
			if test.nativeFailure {
				var exit *exec.ExitError
				if !errors.As(err, &exit) || exit.ExitCode() != 9 {
					t.Fatalf("finalization erased native failure: %v %s", err, output)
				}
			} else if err != nil {
				t.Fatalf("resume: %v %s", err, output)
			}
			manifest := readROSBagWrapperManifest(t, filepath.Dir(prefix))
			if manifest.Finalization != "finalized" || len(manifest.BagFiles) != 1 || manifest.BagFiles[0].ID != "bag."+readableRecordID+".0" || manifest.BagFiles[0].Name != filepath.Base(prefix)+".bag" {
				t.Fatalf("resume changed identity: %+v", manifest)
			}
			if _, err := os.Stat(prefix + ".argv.json"); !errors.Is(err, os.ErrNotExist) {
				t.Fatalf("recovery started a new native recorder: %v", err)
			}
			if _, err := os.Stat(source); !errors.Is(err, os.ErrNotExist) {
				t.Fatalf("source rename unfinished: %v", err)
			}
		})
	}
}

func TestROSBagReadableStandaloneNaturalExitUsesTheSameFinalization(t *testing.T) {
	prefix := filepath.Join(t.TempDir(), "Experiments", "四车_仿真_2026-09-20_18-30-00")
	command := readableROSBagCommand(t, fakeROSBagRuntime(t), prefix)
	command.Env = append(command.Env, "XGC_TEST_ROSBAG_MODE=complete")
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("standalone natural completion: %v %s", err, output)
	}
	manifest := readROSBagWrapperManifest(t, filepath.Dir(prefix))
	if manifest.Status != "completed" || manifest.CapacityLimitReached || manifest.Finalization != "finalized" || len(manifest.BagFiles) != 1 || manifest.BagFiles[0].ID != "bag."+readableRecordID+".0" || manifest.BagFiles[0].Name != filepath.Base(prefix)+".bag" {
		t.Fatalf("no-Session native completion lost files: %+v", manifest)
	}
}
