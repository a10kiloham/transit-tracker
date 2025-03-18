import Layout from "@theme/Layout"
import { ReactNode, useEffect, useRef } from "react"

export default function Configurator(): ReactNode {
  const destinationPath = window.location.hash.replace("#!", "")

  const src = new URL("https://countdown-config.horner.tj")
  src.pathname = destinationPath

  window.location.hash = ""

  return (
    <Layout title="Configurator" wrapperClassName="full-height">
      <iframe
        src={src.href}
        allow="serial; usb; bluetooth; clipboard-write; clipboard-read; geolocation"
        style={{
          height: "100%",
          width: "100%",
          flex: "1",
        }}
      />
    </Layout>
  )
}
