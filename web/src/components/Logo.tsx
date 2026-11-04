interface LogoProps {
  size?: number
  title?: string
  className?: string
}

export function Logo({ size = 28, title, className }: LogoProps) {
  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox="0 0 120 120"
      role={title ? 'img' : undefined}
      aria-hidden={title ? undefined : true}
      aria-label={title}
    >
      <path
        fill="currentColor"
        d="M14 64A46 46 0 0 1 106 64H98A38 38 0 0 0 22 64ZM27 64A33 33 0 0 1 93 64H85A25 25 0 0 0 35 64ZM40 64A20 20 0 0 1 80 64H72A12 12 0 0 0 48 64ZM14 72H106C104 90 86 102 60 102C34 102 16 90 14 72Z"
      />
    </svg>
  )
}
