const status = document.querySelector<HTMLParagraphElement>("#status")!;
document.querySelector<HTMLButtonElement>("#wifi")!.onclick = () => {
  const address = (document.querySelector<HTMLInputElement>("#address")!).value;
  const token = (document.querySelector<HTMLInputElement>("#token")!).value;
  status.textContent = `Wi-Fi connection requested: ${address}?token=${token}`;
};
document.querySelector<HTMLButtonElement>("#usb")!.onclick = () => {
  status.textContent = "Searching for Companion Display USB devices...";
};
document.querySelector<HTMLButtonElement>("#source")!.onclick = () => { status.textContent = "Capture source chooser opens in the native client."; };
document.querySelector<HTMLButtonElement>("#start")!.onclick = () => { status.textContent = "Streaming starts after a transport and capture source are selected."; };
