const Icons = {
  plus: `
	<svg
	  xmlns="http://www.w3.org/2000/svg"
	  width="20"
	  height="20"
	  viewBox="0 0 24 24"
	  fill="none"
	  stroke="currentColor"
	  stroke-width="2"
	  stroke-linecap="round"
	  stroke-linejoin="round"
	>
	  <path d="M12 5v14"/>
	  <path d="M5 12h14"/>
	</svg>
  `,
  
  trash: `
	<svg
	  xmlns="http://www.w3.org/2000/svg"
	  width="20"
	  height="20"
	  viewBox="0 0 24 24"
	  fill="none"
	  stroke="currentColor"
	  stroke-width="2"
	  stroke-linecap="round"
	  stroke-linejoin="round"
	>
	  <path d="M3 6h18"/>
	  <path d="M8 6V4h8v2"/>
	  <path d="M19 6l-1 14H6L5 6"/>
	  <path d="M10 11v6"/>
	  <path d="M14 11v6"/>
	</svg>
  `,
  fanOff: `
	<svg
	  xmlns="http://www.w3.org/2000/svg"
	  width="20"
	  height="20"
	  viewBox="0 0 24 24"
	  fill="none"
	  stroke="currentColor"
	  stroke-width="1.5"
	  stroke-linecap="round"
	  stroke-linejoin="round"
	>
	  <rect x="2" y="2" width="20" height="20" rx="3"/>
	  <circle cx="5" cy="5" r=".5"/>
	  <circle cx="19" cy="5" r=".5"/>
	  <circle cx="5" cy="19" r=".5"/>
	  <circle cx="19" cy="19" r=".5"/>
	  <g transform="translate(12 12) scale(.9) translate(-12 -12)">
		<path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z"/>
		<path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(72 12 12)"/>
		<path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(144 12 12)"/>
		<path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(216 12 12)"/>
		<path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(288 12 12)"/>
	  </g>
	  <circle cx="12" cy="12" r="2" fill="currentColor"/>
	</svg>
  `,

  fanOn: `
	<svg
	  xmlns="http://www.w3.org/2000/svg"
	  width="20"
	  height="20"
	  viewBox="0 0 24 24"
	  fill="none"
	  stroke="currentColor"
	  stroke-width="1.5"
	  stroke-linecap="round"
	  stroke-linejoin="round"
	>
	  <rect x="2" y="2" width="20" height="20" rx="3"/>
	  <circle cx="5" cy="5" r=".5"/>
	  <circle cx="19" cy="5" r=".5"/>
	  <circle cx="5" cy="19" r=".5"/>
	  <circle cx="19" cy="19" r=".5"/>
	  <g class="fan-spin">
		<g transform="translate(12 12) scale(.9) translate(-12 -12)" fill="currentColor" fill-opacity="0.3">
		  <path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z"/>
		  <path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(72 12 12)"/>
		  <path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(144 12 12)"/>
		  <path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(216 12 12)"/>
		  <path d="M12 9.8C9.6 8.6 9.4 5 12.6 3.2C14.4 5.6 14 8.2 12.9 9.9Z" transform="rotate(288 12 12)"/>
		</g>
	  </g>
	  <circle cx="12" cy="12" r="2" fill="currentColor"/>
	</svg>
  `  
};