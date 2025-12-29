#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

def run_command(cmd, env=None, check=True):
    """Run a subprocess command and print output."""
    print(f"--> Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, env=env, text=True, capture_output=True)
    if check and result.returncode != 0:
        print(f"Error running command: {cmd}")
        print("STDOUT:", result.stdout)
        print("STDERR:", result.stderr)
        sys.exit(1)
    return result



def main():
    # Ensure we are in the project root
    project_root = Path(__file__).resolve().parent.parent
    os.chdir(project_root)
    print(f"Project Root: {project_root}")

    # Use a temporary directory as the install prefix
    with tempfile.TemporaryDirectory() as temp_prefix:
        print(f"Using temporary prefix: {temp_prefix}")
        
        # Define a unique service name for testing to avoid collisions
        test_service_name = "com.mob.lotab.test_" + str(int(time.time()))
        
        env = os.environ.copy()
        env["PREFIX"] = temp_prefix
        # Tell the script which service name to use
        env["LOTAB_SERVICE_NAME"] = test_service_name
        
        # 1. Build and Install
        print("\n=== Step 1: Build and Install ===\n")
        # We assume build.sh handles reconfiguration properly when PREFIX changes
        run_command(["./build.sh", "install"], env=env)

        # Verify files were installed to tmp
        if not (Path(temp_prefix) / "bin/lotab_daemon").exists():
            print("FAILURE: daemon binary not found in install prefix")
            sys.exit(1)
            
        # --- TEST FIXUP: Rename/Patch Plist for Test Service Name ---
        # The build installed 'com.mob.lotab.plist'. We need it to be 'com.mob.lotab.test_XXX.plist'
        # and contain the matching Label.
        share_dir = Path(temp_prefix) / "share/lotab"
        orig_plist = share_dir / "com.mob.lotab.plist"
        new_plist = share_dir / f"{test_service_name}.plist"
        
        if orig_plist.exists():
            print(f"Patching plist for test service: {test_service_name}")
            content = orig_plist.read_text()
            # Replace Label
            content = content.replace("<string>com.mob.lotab</string>", f"<string>{test_service_name}</string>")
            new_plist.write_text(content)
            # Remove original to avoid confusion, though script looks for PLIST_NAME
            os.remove(orig_plist)
        else:
            print(f"FAILURE: Original plist not found at {orig_plist}")
            sys.exit(1)
        # ------------------------------------------------------------
        
        # 2. Load Service
        print("\n=== Step 2: Load Service ===\n")
        launchctl_script = "./scripts/launchctl.sh"
        run_command([launchctl_script], env=env)
        
        # Give launchd a moment
        time.sleep(1)
        
        # 3. Verify Loaded
        print("\n=== Step 3: Verify Service Loaded ===\n")
        # Check specific test service name
        check_test_service_status(test_service_name, should_exist=True)
        
        # 4. Unload Service
        print("\n=== Step 4: Unload Service ===\n")
        run_command([launchctl_script, "unload"], env=env)
        
        # 5. Verify Unloaded
        print("\n=== Step 5: Verify Service Unloaded ===\n")
        check_test_service_status(test_service_name, should_exist=False)
        
        # 6. Verify Cleanup (Plist removal)
        # launchctl.sh unload should remove the plist from ~/Library/LaunchAgents
        plist_path = Path.home() / f"Library/LaunchAgents/{test_service_name}.plist"
        if plist_path.exists():
            print(f"FAILURE: Plist file still exists at {plist_path}")
            sys.exit(1)
        print(f"Cleanup confirmed: {plist_path} removed.")

    print("\nSUCCESS: Install verification verification passed!")

def check_test_service_status(service_name, should_exist=True):
    """Check if the service is present in launchctl list."""
    result = run_command(["launchctl", "list"], check=False)
    found = service_name in result.stdout
    
    if should_exist and not found:
        print(f"FAILURE: Service {service_name} not found in launchctl list")
        sys.exit(1)
    elif not should_exist and found:
        print(f"FAILURE: Service {service_name} still found in launchctl list")
        sys.exit(1)
    else:
        status_msg = "found" if found else "not found"
        print(f"Service status confirmed: {status_msg} (Expected: {should_exist})")

if __name__ == "__main__":
    main()
